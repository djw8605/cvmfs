/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_CACHE_H_
#define CVMFS_CACHE_H_

#include <stdint.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

#include "atomic.h"
#include "backoff.h"
#include "catalog_mgr.h"
#include "file_chunk.h"
#include "gtest/gtest_prod.h"
#include "manifest_fetch.h"
#include "shortstring.h"
#include "signature.h"
#include "util.h"
#include "statistics.h"

namespace catalog {
class DirectoryEntry;
class Catalog;
}

namespace hash {
struct Any;
}

namespace download {
class DownloadManager;
}

class QuotaManager;

namespace cache {

enum CacheModes {
  kCacheReadWrite = 0,
  kCacheReadOnly,
};


/**
 * The Cache Manager provides (virtual) file descriptors to content-addressable
 * objects in the cache.  The implementation can use a POSIX file system or
 * other means such as a key-value store.  A file descriptor must remain
 * readable until closed, no matter if it is removed from the backend storage
 * or not (POSIX semantics).
 *
 * Writing into the cache is streamed and transactional: a file descriptor must
 * be acquired from StartTxn and the object is only visible in the cache after
 * CommitTxn.  The state of the transaction is carried in an opque transaction
 * object, which needs to be provided by the caller.  The size of the object is
 * returned by SizeOfTxn.  This way, users of derived classes can take care of
 * the storage allocation (e.g. on the stack), while the derived class
 * determines the contents of the transaction object.  For race-free read access
 * to objects that are just being written to the cache, the OpenFromTxn routine
 * is used just before the transaction is committed.
 *
 * Writing to the cache can be coupled to a quota manager.  The quota manager
 * maintains some extra information for data chunks: whether they are
 * volatile, whether they are pinned, and a description (usually the path that
 * corresponds to a data chunk).  By default the NoopQuotaManager is used, which
 * ignores all this extra information.  The CtrlTxn() function is used to
 * specify this extra information sometime between StartTxn() and CommitTxn().
 *
 * The integer return values can be negative and, in this case, represent a
 * -errno failure code.  Otherwise the return value 0 indicates a success, or
 * >= 0 for a file descriptor.
 */
class CacheManager : SingleCopy {
 public:
  /**
   * Sizes of objects should be known for StartTxn().  For file catalogs we
   * cannot ensure that, however, because the size field for nested catalogs
   * was only recently added.
   */
  static const uint64_t kSizeUnknown;

  /**
   * Relevant for the quota management
   */
  enum ObjectType {
    kTypeRegular = 0,
    kTypeCatalog,  // implies pinned
    kTypePinned,
    kTypeVolatile,
  };

  virtual ~CacheManager();
  virtual int Open(const shash::Any &id) = 0;
  virtual int64_t GetSize(int fd) = 0;
  virtual int Close(int fd) = 0;
  virtual int64_t Pread(int fd, void *buf, uint64_t size, uint64_t offset) = 0;
  virtual int Dup(int fd) = 0;

  virtual uint16_t SizeOfTxn() = 0;
  virtual int StartTxn(const shash::Any &id, uint64_t size, void *txn) = 0;
  virtual void CtrlTxn(const std::string &description,
                       const ObjectType type,
                       const int flags,  // reserved for future use
                       void *txn) = 0;
  virtual int64_t Write(const void *buf, uint64_t sz, void *txn) = 0;
  virtual int Reset(void *txn) = 0;
  virtual int AbortTxn(void *txn) = 0;
  virtual int OpenFromTxn(void *txn) = 0;
  virtual int CommitTxn(void *txn) = 0;

  bool Open2Mem(const shash::Any &id, unsigned char **buffer, uint64_t *size);
  bool CommitFromMem(const shash::Any &id,
                     const unsigned char *buffer,
                     const uint64_t size,
                     const std::string &description);

  QuotaManager *quota_mgr() { return quota_mgr_; }

 protected:
  CacheManager();

  /**
   * Never NULL but defaults to NoopQuotaManager.
   */
  QuotaManager *quota_mgr_;
};


/**
 * Cache manger implementation using a file system (cache directory) as a
 * backing storage.
 */
class PosixCacheManager : public CacheManager {
  FRIEND_TEST(T_CacheManager, CommitTxnQuotaNotifications);
  FRIEND_TEST(T_CacheManager, CommitTxnRenameFail);
  FRIEND_TEST(T_CacheManager, Open);
  FRIEND_TEST(T_CacheManager, OpenFromTxn);
  FRIEND_TEST(T_CacheManager, Rename);
  FRIEND_TEST(T_CacheManager, StartTxn);
  FRIEND_TEST(T_CacheManager, TearDown2ReadOnly);

 public:
  /**
   * As of 25M, a file is considered a "big file", which means it is dangerous
   * to apply asynchronous semantics.  On start of a transaction with a big file
   * the cache is cleaned up opportunistically.
   */
  static const uint64_t kBigFile;

  static PosixCacheManager *Create(const std::string &cache_path,
                                   const bool alien_cache);
  virtual	~PosixCacheManager() { };

  virtual int Open(const shash::Any &id);
  virtual int64_t GetSize(int fd);
  virtual int Close(int fd);
  virtual int64_t Pread(int fd, void *buf, uint64_t size, uint64_t offset);
  virtual int Dup(int fd);

  virtual uint16_t SizeOfTxn() { return sizeof(Transaction); }
  virtual int StartTxn(const shash::Any &id, uint64_t size, void *txn);
  virtual void CtrlTxn(const std::string &description,
                       const ObjectType type,
                       const int flags,
                       void *txn);
  virtual int64_t Write(const void *buf, uint64_t size, void *txn);
  virtual int Reset(void *txn);
  virtual int OpenFromTxn(void *txn);
  virtual int AbortTxn(void *txn);
  virtual int CommitTxn(void *txn);

  void TearDown2ReadOnly();

 private:
  enum CacheModes {
    kCacheReadWrite = 0,
    kCacheReadOnly,
  };

  struct Transaction {
    Transaction(const shash::Any &id, const std::string &final_path)
      : buf_pos(0)
      , size(0)
      , expected_size(kSizeUnknown)
      , fd(-1)
      , type(kTypeRegular)
      , tmp_path()
      , final_path(final_path)
      , id(id)
    { }

    unsigned char buffer[4096];
    unsigned buf_pos;
    uint64_t size;
    uint64_t expected_size;
 	  int fd;
    ObjectType type;
    std::string tmp_path;
    std::string final_path;
    std::string description;
    shash::Any id;
  };

  PosixCacheManager(const std::string &cache_path, const bool alien_cache)
    : cache_path_(cache_path)
    , txn_template_path_(cache_path_ + "/txn/fetchXXXXXX")
    , alien_cache_(alien_cache)
    , alien_cache_on_nfs_(false)
    , cache_mode_(kCacheReadWrite)
    , reports_correct_filesize_(true)
  {
    atomic_init32(&no_inflight_txns_);
  }

  std::string GetPathInCache(const shash::Any &id);
  int Rename(const char *oldpath, const char *newpath);
  int Flush(Transaction *transaction);

  std::string cache_path_;
  std::string txn_template_path_;
  bool alien_cache_;
  bool alien_cache_on_nfs_;
  CacheModes cache_mode_;

  /**
   * The cache can only degrade to a read-only cache once all writable file
   * descriptors from transactions are closed.  This is indicated by a zero
   * value in this variable.
   */
  atomic_int32 no_inflight_txns_;

  /**
   * Hack for HDFS which writes file sizes asynchronously.
   */
  bool reports_correct_filesize_;
};


bool Init(const std::string &cache_path, const bool alien_cache);
void Fini();

int Open(const shash::Any &id);
int FetchDirent(const catalog::DirectoryEntry &d,
                const std::string &cvmfs_path,
                const bool volatile_content,
                download::DownloadManager *download_manager);
int FetchChunk(const FileChunk &chunk,
               const std::string &cvmfs_path,
               const bool volatile_content,
               download::DownloadManager *download_manager);
int64_t GetNumDownloads();

CacheModes GetCacheMode();
void TearDown2ReadOnly();


/**
 * A catalog manager that fetches its catalogs remotely and stores
 * them in the cache.
 */
class CatalogManager : public catalog::AbstractCatalogManager {
  friend class ManifestEnsemble;  // Maintains certificate hit/miss counters

 public:
  CatalogManager(const std::string &repo_name,
                 signature::SignatureManager *signature_manager,
                 download::DownloadManager *download_manager,
                 perf::Statistics *statistics);
  virtual ~CatalogManager();

  bool InitFixed(const shash::Any &root_hash);

  shash::Any GetRootHash() {
    ReadLock();
    shash::Any result = mounted_catalogs_[PathString("", 0)];
    Unlock();
    return result;
  }
  bool offline_mode() const { return offline_mode_; }
  uint64_t all_inodes() const { return all_inodes_; }
  uint64_t loaded_inodes() const { return loaded_inodes_; }

 protected:
  catalog::LoadError LoadCatalog(const PathString  &mountpoint,
                                 const shash::Any  &hash,
                                 std::string       *catalog_path,
                                 shash::Any        *catalog_hash);
  void UnloadCatalog(const catalog::Catalog *catalog);
  catalog::Catalog* CreateCatalog(const PathString &mountpoint,
                                  const shash::Any  &catalog_hash,
                                  catalog::Catalog *parent_catalog);
  void ActivateCatalog(catalog::Catalog *catalog);

 private:
  catalog::LoadError LoadCatalogCas(const shash::Any &hash,
                                    const std::string &cvmfs_path,
                                    std::string *catalog_path);

  /**
   * required for unpinning
   */
  std::map<PathString, shash::Any> loaded_catalogs_;
  std::map<PathString, shash::Any> mounted_catalogs_;

  std::string repo_name_;
  signature::SignatureManager *signature_manager_;
  download::DownloadManager *download_manager_;
  bool offline_mode_;  /**< cached copy used because there is no network */
  perf::Counter *n_certificate_hits_;
  perf::Counter *n_certificate_misses_;
  uint64_t all_inodes_;
  uint64_t loaded_inodes_;
  BackoffThrottle backoff_throttle_;
};


/**
 * Tries to fetch the certificate from cache
 */
class ManifestEnsemble : public manifest::ManifestEnsemble {
 public:
  explicit ManifestEnsemble(cache::CatalogManager *catalog_mgr) {
    catalog_mgr_ = catalog_mgr;
  }
  void FetchCertificate(const shash::Any &hash);
 private:
  cache::CatalogManager *catalog_mgr_;
};

}  // namespace cache

#endif  // CVMFS_CACHE_H_
