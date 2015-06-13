/**
 * This file is part of the CernVM File System.
 *
 * The cache module maintains the local file cache.  Files are
 * staged into the cache by Fetch().  The cache stores files with a name
 * according to their content hash.
 *
 * The procedure is
 *   -# Look in the catalog for content hash
 *   -# If it is in local cache: return file descriptor
 *   -# Otherwise download, store in cache and return fd
 *
 * Each running CVMFS instance has to have a separate cache directory.
 * The local cache directory (directories 00..ff) can be accessed
 * in parallel to a running CVMFS, i.e. files can be deleted for instance
 * anytime.  However, this will confuse the cache database managed by the lru
 * module.
 *
 * Files are created in txn directory first.  At the very latest
 * point they are renamed into their "real" content hash names atomically by
 * rename().  This concept is taken over from GROW-FS.
 *
 * Identical URLs won't be concurrently downloaded.  The first thread performs
 * the download and informs the other, waiting threads on pipes.
 */

#define __STDC_FORMAT_MACROS

#include "cvmfs_config.h"
#include "cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef __APPLE__
#include <sys/statfs.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "atomic.h"
#include "compression.h"
#include "cvmfs.h"
#include "directory_entry.h"
#include "download.h"
#include "hash.h"
#include "logging.h"
#include "manifest.h"
#include "manifest_fetch.h"
#include "platform.h"
#include "quota.h"
#include "shortstring.h"
#include "signature.h"
#include "smalloc.h"
#include "statistics.h"
#include "util.h"

#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

using namespace std;  // NOLINT

namespace cache {

uint64_t kBigFile = 25*1024*1024;  // As of 25M, a file is considered "big file"


/**
 * A CallGuard object can be placed at the beginning of a function.  It counts
 * the number of so-annotated functions that are in flight.  The Drainout() call
 * will wait until all functions that have been called so far are finished.
 *
 * The class is used in order to wait for remaining calls when switching into
 * the read-only cache mode.
 */
class CallGuard {
 public:
  CallGuard() {
    int32_t global_drainout = atomic_read32(&global_drainout_);
    drainout_ = (global_drainout != 0);
    if (!drainout_)
      atomic_inc32(&num_inflight_calls_);
  }
  ~CallGuard() {
    if (!drainout_)
      atomic_dec32(&num_inflight_calls_);
  }
  static void Drainout() {
    atomic_cas32(&global_drainout_, 0, 1);
    while (atomic_read32(&num_inflight_calls_) != 0)
      SafeSleepMs(50);
  }
 private:
  bool drainout_;
  static atomic_int32 global_drainout_;
  static atomic_int32 num_inflight_calls_;
};
atomic_int32 CallGuard::num_inflight_calls_ = 0;
atomic_int32 CallGuard::global_drainout_ = 0;


//------------------------------------------------------------------------------

const uint64_t CacheManager::kSizeUnknown = uint64_t(-1);


CacheManager::CacheManager() : quota_mgr_(new NoopQuotaManager()) { }


CacheManager::~CacheManager() {
  delete quota_mgr_;
}


/**
 * Tries to open a file and copies its contents into a newly malloc'd
 * memory area.  User of the function has to free buffer (if successful).
 *
 * @param[in] id content hash of the catalog entry.
 * @param[out] buffer Contents of the file
 * @param[out] size Size of the file
 * \return True if successful, false otherwise.
 */
bool CacheManager::Open2Mem(
  const shash::Any &id,
  unsigned char **buffer,
  uint64_t *size)
{
  *size = 0;
  *buffer = NULL;

  int fd = this->Open(id);
  if (fd < 0)
    return false;

  int64_t s = this->GetSize(fd);
  assert(s >= 0);
  *size = static_cast<uint64_t>(s);

  int64_t retval = 0;
  if (*size > 0) {
    *buffer = static_cast<unsigned char *>(smalloc(*size));
    retval = this->Pread(fd, *buffer, *size, 0);
  } else {
    *buffer = NULL;
  }

  this->Close(fd);
  if ((retval < 0) || (static_cast<uint64_t>(retval) != *size)) {
    free(*buffer);
    *buffer = NULL;
    *size = 0;
    return false;
  }
  return true;
}


/**
 * Commits the memory blob buffer to the given chunk id.  No checking!
 * The hash and the memory blob need to match.
 */
bool CacheManager::CommitFromMem(
  const shash::Any &id,
  const unsigned char *buffer,
  const uint64_t size,
  const string &description)
{
  void *txn = alloca(this->SizeOfTxn());
  int fd = this->StartTxn(id, size, txn);
  if (fd < 0)
    return false;
  this->CtrlTxn(description, kTypeRegular, 0, txn);
  int64_t retval = this->Write(buffer, size, txn);
  if ((retval < 0) || (static_cast<uint64_t>(retval) != size)) {
		this->AbortTxn(txn);
    return false;
  }
  retval = this->CommitTxn(txn);
  return retval == 0;
}


//------------------------------------------------------------------------------

const uint64_t PosixCacheManager::kBigFile = 25*1024*1024;  // 25M


int PosixCacheManager::AbortTxn(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  LogCvmfs(kLogCache, kLogDebug, "abort %s", transaction->tmp_path.c_str());
  close(transaction->fd);
  int result = unlink(transaction->tmp_path.c_str());
  transaction->~Transaction();
  atomic_dec32(&no_inflight_txns_);
  if (result == -1)
    return -errno;
  return 0;
}


int PosixCacheManager::Close(int fd) {
  int retval = close(fd);
  if (retval != 0)
    return -errno;
  return 0;
}


int PosixCacheManager::CommitTxn(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  int result;
  LogCvmfs(kLogCache, kLogDebug, "commit %s %s",
           transaction->final_path.c_str(), transaction->tmp_path.c_str());

  result = Flush(transaction);
  close(transaction->fd);
  if (result < 0) {
    unlink(transaction->tmp_path.c_str());
    transaction->~Transaction();
    atomic_dec32(&no_inflight_txns_);
    return result;
  }

  // To support debugging, move files into quarantine on file size mismatch
  if (transaction->size != transaction->expected_size) {
    // Allow size to be zero if alien cache, because hadoop-fuse-dfs returns
    // size zero for a while
    if ( (transaction->expected_size != kSizeUnknown) &&
         (reports_correct_filesize_ || (transaction->size != 0)) )
    {
      LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
               "size check failure for %s, expected %lu, got %lu",
               transaction->id.ToString().c_str(),
               transaction->expected_size, transaction->size);
      CopyPath2Path(transaction->tmp_path,
                    cache_path_ + "/quarantaine/" + transaction->id.ToString());
      unlink(transaction->tmp_path.c_str());
      transaction->~Transaction();
      atomic_dec32(&no_inflight_txns_);
      return -EIO;
    }
  }

  if ((transaction->type == kTypePinned) || (transaction->type == kTypeCatalog))
  {
    bool retval = quota_mgr_->Pin(
      transaction->id, transaction->size, transaction->description,
      (transaction->type == kTypeCatalog));
    if (!retval) {
      LogCvmfs(kLogCache, kLogDebug, "commit failed: cannot pin %s",
               transaction->id.ToString().c_str());
      unlink(transaction->tmp_path.c_str());
      transaction->~Transaction();
      atomic_dec32(&no_inflight_txns_);
      return -ENOSPC;
    }
  }

  // Move the temporary file into its final location
  if (alien_cache_) {
    int retval = chmod(transaction->tmp_path.c_str(), 0660);
    assert(retval == 0);
  }
  result =
    Rename(transaction->tmp_path.c_str(), transaction->final_path.c_str());
  if (result < 0) {
    LogCvmfs(kLogCache, kLogDebug, "commit failed: %s", strerror(errno));
    unlink(transaction->tmp_path.c_str());
    if ((transaction->type == kTypePinned) ||
        (transaction->type == kTypeCatalog))
    {
      quota_mgr_->Remove(transaction->id);
    }
  } else {
    // Success, inform quota manager
    if (transaction->type == kTypeVolatile) {
      quota_mgr_->InsertVolatile(transaction->id, transaction->size,
                                 transaction->description);
    } else if (transaction->type == kTypeRegular) {
      quota_mgr_->Insert(transaction->id, transaction->size,
                         transaction->description);
    }
  }
  transaction->~Transaction();
  atomic_dec32(&no_inflight_txns_);
  return result;
}


PosixCacheManager *PosixCacheManager::Create(
  const string &cache_path,
  const bool alien_cache)
{
  UniquePtr<PosixCacheManager> cache_manager(
    new PosixCacheManager(cache_path, alien_cache));
  assert(cache_manager.IsValid());

  if (cache_manager->alien_cache_) {
    if (!MakeCacheDirectories(cache_path, 0770)) {
      return NULL;
    }
    LogCvmfs(kLogCache, kLogDebug | kLogSyslog,
             "Cache directory structure created.");
    struct statfs cache_buf;
    if ((statfs(cache_path.c_str(), &cache_buf) == 0) &&
        (cache_buf.f_type == NFS_SUPER_MAGIC))
    {
      cache_manager->alien_cache_on_nfs_ = true;
      LogCvmfs(kLogCache, kLogDebug | kLogSyslog,
             "Alien cache is on NFS.");
    }
  } else {
    if (!MakeCacheDirectories(cache_path, 0700))
      return NULL;
  }

  if (FileExists(cache_path + "/cvmfscatalog.cache")) {
    LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
             "Not mounting on cvmfs 2.0.X cache");
    return NULL;
  }

  return cache_manager.Release();
}


void PosixCacheManager::CtrlTxn(
  const std::string &description,
  const ObjectType type,
  const int flags,
  void *txn)
{
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  transaction->description = description;
  transaction->type = type;
}


int PosixCacheManager::Dup(int fd) {
  int new_fd = dup(fd);
  if (new_fd < 0)
    return -errno;
  return new_fd;
}


int PosixCacheManager::Flush(Transaction *transaction) {
  if (transaction->buf_pos == 0)
    return 0;
  int written =
    write(transaction->fd, transaction->buffer, transaction->buf_pos);
  if (written < 0)
    return -errno;
  if (static_cast<unsigned>(written) != transaction->buf_pos) {
    transaction->buf_pos -= written;
    return -EIO;
  }
  transaction->buf_pos = 0;
  return 0;
}


inline string PosixCacheManager::GetPathInCache(const shash::Any &id) {
  return cache_path_ + "/" + id.MakePathWithoutSuffix();
}


int64_t PosixCacheManager::GetSize(int fd) {
  platform_stat64 info;
  int retval = platform_fstat(fd, &info);
  if (retval != 0)
    return -errno;
  return info.st_size;
}


int PosixCacheManager::Open(const shash::Any &id) {
  const string path = GetPathInCache(id);
  int result = open(path.c_str(), O_RDONLY);

  if (result >= 0) {
    LogCvmfs(kLogCache, kLogDebug, "hit %s", path.c_str());
    // platform_disable_kcache(result);
    quota_mgr_->Touch(id);
  } else {
    result = -errno;
    LogCvmfs(kLogCache, kLogDebug, "miss %s (%d)", path.c_str(), result);
  }
  return result;
}


int PosixCacheManager::OpenFromTxn(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  int retval = Flush(transaction);
  if (retval < 0)
    return retval;
  int fd_rdonly = open(transaction->tmp_path.c_str(), O_RDONLY);
  if (fd_rdonly == -1)
    return -errno;
  return fd_rdonly;
}


int64_t PosixCacheManager::Pread(
  int fd,
  void *buf,
  uint64_t size,
  uint64_t offset)
{
  int64_t result = pread(fd, buf, size, offset);
  if (result < 0)
    return -errno;
  return result;
}


int PosixCacheManager::Rename(const char *oldpath, const char *newpath) {
  int result;
  if (!alien_cache_on_nfs_) {
    result = rename(oldpath, newpath);
    if (result < 0)
      return -errno;
    return 0;
  }

  result = link(oldpath, newpath);
  if (result < 0) {
    if (errno == EEXIST)
      LogCvmfs(kLogCache, kLogDebug, "%s already existed, ignoring", newpath);
    else
      return -errno;
  }
  result = unlink(oldpath);
  if (result < 0)
    return -errno;
  return 0;
}


int PosixCacheManager::Reset(void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);
  transaction->buf_pos = 0;
  transaction->size = 0;
  int retval = lseek(transaction->fd, 0, SEEK_SET);
  if (retval < 0)
    return -errno;
  retval = ftruncate(transaction->fd, 0);
  if (retval < 0)
    return -errno;
  return 0;
}


int PosixCacheManager::StartTxn(
  const shash::Any &id,
  uint64_t size,
  void *txn)
{
  atomic_inc32(&no_inflight_txns_);
  if (cache_mode_ == kCacheReadOnly) {
    atomic_dec32(&no_inflight_txns_);
    return -EROFS;
  }

  if (size != kSizeUnknown) {
    if (size > quota_mgr_->GetMaxFileSize()) {
      LogCvmfs(kLogCache, kLogDebug, "file too big for lru cache (%"PRIu64" "
                                     "requested but only %"PRIu64" bytes free)",
               size, quota_mgr_->GetMaxFileSize());
      atomic_dec32(&no_inflight_txns_);
      return -ENOSPC;
    }

    // Opportunistically clean up cache for large files
    if (size > kBigFile) {
      assert(quota_mgr_->GetCapacity() >= size);
      quota_mgr_->Cleanup(quota_mgr_->GetCapacity() - size);
    }
  }

  Transaction *transaction = new (txn) Transaction(id, GetPathInCache(id));
  const unsigned temp_path_len = txn_template_path_.length();

  char template_path[temp_path_len + 1];
  memcpy(template_path, &txn_template_path_[0], temp_path_len);
  template_path[temp_path_len] = '\0';
  transaction->fd = mkstemp(template_path);
  if (transaction->fd == -1) {
    transaction->~Transaction();
    atomic_dec32(&no_inflight_txns_);
    return -errno;
  }

  LogCvmfs(kLogCache, kLogDebug, "start transaction on %s has result %d",
           template_path, transaction->fd);
  transaction->tmp_path = template_path;
  transaction->expected_size = size;
  return transaction->fd;
}


void PosixCacheManager::TearDown2ReadOnly() {
  cache_mode_ = kCacheReadOnly;
  while (atomic_read32(&no_inflight_txns_) != 0)
    SafeSleepMs(50);

  QuotaManager *old_manager = quota_mgr_;
  quota_mgr_ = new NoopQuotaManager();
  delete old_manager;

  // TODO(jblomer): Hacks, should be handled elsewhere
  unlink(("running." + *cvmfs::repository_name_).c_str());
  LogCvmfs(kLogCache, kLogSyslog, "switch to read-only cache mode");
  SetLogMicroSyslog("");
}


int64_t PosixCacheManager::Write(const void *buf, uint64_t size, void *txn) {
  Transaction *transaction = reinterpret_cast<Transaction *>(txn);

  if (transaction->expected_size != kSizeUnknown) {
    if (transaction->size + size > transaction->expected_size)
      return -ENOSPC;
  }

  uint64_t written = 0;
  while (written < size) {
    if (transaction->buf_pos == sizeof(transaction->buffer)) {
      int retval = Flush(transaction);
      if (retval != 0) {
        transaction->size += written;
        return retval;
      }
    }
    uint64_t remaining = size - written;
    uint64_t space_in_buffer =
      sizeof(transaction->buffer) - transaction->buf_pos;
    uint64_t batch_size = std::min(remaining, space_in_buffer);
    memcpy(transaction->buffer + transaction->buf_pos, buf, batch_size);
    transaction->buf_pos += batch_size;
    written += batch_size;
  }
  transaction->size += written;
  return written;
}


//------------------------------------------------------------------------------


/**
 * Everything that should be reused per thread
 */
struct ThreadLocalStorage {
  int pipe_wait[2];
  vector<int> other_pipes_waiting;
  download::JobInfo download_job;
};

typedef map< shash::Any, vector<int> * > ThreadQueues;

string *cache_path_ = NULL;
bool alien_cache_ = false;
bool alien_cache_on_nfs_ = false;
/**
 * Maps currently downloaded chunks to an array of writer's ends of a pipe to
 * signal the waiting threads when the download has finished.
 */
ThreadQueues *queues_download_ = NULL;
pthread_mutex_t lock_queues_download_ = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t thread_local_storage_;
vector<ThreadLocalStorage *> *tls_blocks_;
pthread_mutex_t lock_tls_blocks_ = PTHREAD_MUTEX_INITIALIZER;
atomic_int64 num_download_;

CacheModes cache_mode_;


static void CleanupTLS(ThreadLocalStorage *tls) {
  close(tls->pipe_wait[0]);
  close(tls->pipe_wait[1]);
  delete tls;
}


static void TLSDestructor(void *data) {
  ThreadLocalStorage *tls = static_cast<ThreadLocalStorage *>(data);
  pthread_mutex_lock(&lock_tls_blocks_);
  for (vector<ThreadLocalStorage *>::iterator i = tls_blocks_->begin(),
       iEnd = tls_blocks_->end(); i != iEnd; ++i)
  {
    if ((*i) == tls) {
      tls_blocks_->erase(i);
      break;
    }
  }
  pthread_mutex_unlock(&lock_tls_blocks_);
  CleanupTLS(tls);
}


/**
 * Initializes the cache directory with the 256 subdirectories and /txn.
 *
 * \return True on success, false otherwise
 */
bool Init(const string &cache_path, const bool alien_cache) {
  cache_mode_ = kCacheReadWrite;
  cache_path_ = new string(cache_path);
  alien_cache_ = alien_cache;
  queues_download_ = new ThreadQueues();
  tls_blocks_ = new vector<ThreadLocalStorage *>();
  atomic_init64(&num_download_);

  if (alien_cache_) {
    if (!MakeCacheDirectories(cache_path, 0770)) {
      return false;
    }
    LogCvmfs(kLogCache, kLogDebug | kLogSyslog,
             "Cache directory structure created.");
    struct statfs cache_buf;
    if ((statfs(cache_path.c_str(), &cache_buf) == 0) &&
        (cache_buf.f_type == NFS_SUPER_MAGIC))
    {
      alien_cache_on_nfs_ = true;
      LogCvmfs(kLogCache, kLogDebug | kLogSyslog,
             "Alien cache is on NFS.");
    }
  } else {
    if (!MakeCacheDirectories(cache_path, 0700))
      return false;
  }

  if (FileExists(cache_path + "/cvmfscatalog.cache")) {
    LogCvmfs(kLogCache, kLogStderr | kLogSyslogErr,
             "Not mounting on cvmfs 2.0.X cache");
    return false;
  }

  int retval = pthread_key_create(&thread_local_storage_, TLSDestructor);
  assert(retval == 0);

  return true;
}


void Fini() {
  pthread_mutex_lock(&lock_tls_blocks_);
  for (unsigned i = 0; i < tls_blocks_->size(); ++i)
    CleanupTLS((*tls_blocks_)[i]);
  pthread_mutex_unlock(&lock_tls_blocks_);
  pthread_key_delete(thread_local_storage_);
  delete cache_path_;
  delete queues_download_;
  delete tls_blocks_;
  cache_path_ = NULL;
  queues_download_ = NULL;
  tls_blocks_ = NULL;
}


CacheModes GetCacheMode() {
  return cache_mode_;
}

void TearDown2ReadOnly() {
  cache_mode_ = kCacheReadOnly;
  CallGuard::Drainout();
  // TODO-QUOTAMGR
  //  quota::Fini();
  //  unlink(("running." + *cvmfs::repository_name_).c_str());
  LogCvmfs(kLogCache, kLogSyslog, "switch to read-only cache mode");
  SetLogMicroSyslog("");
}


/**
 * Transforms a catalog entry into a name for local cache.
 *
 * @param[in] id content hash of the catalog entry.
 * \return Absolute path in local cache.
 */
static inline string GetPathInCache(const shash::Any &id) {
  return *cache_path_ + "/" + id.MakePathWithoutSuffix();
}


/**
 * Transform a catalog entry into a temporary name in txn-directory.
 *
 * @param[in] id content hash of the catalog entry.
 * \return Absolute path in local cache txn-directory.
 */
static inline string GetTempName()
{
  return *cache_path_ + "/txn/" + "fetchXXXXXX";
}


/**
 * Tries to open a catalog entry in local cache.
 *
 * @param[in] id content hash of the catalog entry.
 * \return A file descriptor if file is in cache.  Error code of open() else.
 */
int Open(const shash::Any &id) {
  const string path = GetPathInCache(id);
  int result = ::open(path.c_str(), O_RDONLY);

  if (result >= 0) {
    LogCvmfs(kLogCache, kLogDebug, "hit %s", path.c_str());
    // platform_disable_kcache(result);
  } else {
    result = -errno;
    LogCvmfs(kLogCache, kLogDebug, "miss %s (%d)", path.c_str(), result);
  }

  return result;
}


/**
 * Tries to open a file and copies its contents into a newly malloced
 * memory area.  User of the function has to free buffer (if successful).
 *
 * @param[in] id content hash of the catalog entry.
 * @param[out] buffer Contents of the file
 * @param[out] size Size of the file
 * \return True if successful, false otherwise.
 */
static bool Open2Mem(const shash::Any &id,
                     unsigned char **buffer, uint64_t *size)
{
  *size = 0;
  *buffer = NULL;

  int fd = cache::Open(id);
  if (fd < 0)
    return false;

  platform_stat64 info;
  if (platform_fstat(fd, &info) != 0) {
    close(fd);
    return false;
  }

  *size = info.st_size;
  *buffer = static_cast<unsigned char *>(smalloc(*size));

  int64_t retval = read(fd, *buffer, *size);
  if ((retval < 0) || (static_cast<uint64_t>(retval) != *size)) {
    close(fd);
    free(*buffer);
    *buffer = NULL;
    *size = 0;
    return false;
  }

  close(fd);
  return true;
}


/**
 * Starts a "transaction" based on a catalog entry, i.e. start download in a
 * temporary file.
 *
 * @param[in] id content hash of the catalog entry.
 * @param[out] path Absolute path of the file in local cache after commit
 * @param[out] temp_path Absolute path of the temporoary file in local cache
 * \return File descriptor of temporary file, error code of mkstemp() else
 */
static int StartTransaction(const shash::Any &id,
                            string *final_path, string *temp_path)
{
  if (cache_mode_ == kCacheReadOnly)
    return -EROFS;

  int result;
  *final_path = GetPathInCache(id);
  *temp_path = GetTempName();
  const unsigned temp_path_length = temp_path->length();

  char template_path[temp_path_length + 1];
  memcpy(template_path, &(*temp_path)[0], temp_path_length);
  template_path[temp_path_length] = '\0';
  result = ::mkstemp(template_path);
  if (result == -1)
    result = -errno;

  LogCvmfs(kLogCache, kLogDebug, "start transaction on %s has result %d",
           template_path, result);

  *temp_path = template_path;
  return result;
}


/**
 * Aborts a file download started with StartTransaction() and cleans
 * temporoary storage.
 *
 * @param[in] temp_path Absolute path of the temporoary file in local cache
 * \return Zero on success, error code of unlink() else.
 */
static int AbortTransaction(const string &temp_path) {
  LogCvmfs(kLogCache, kLogDebug, "abort %s", temp_path.c_str());

  int result = unlink(temp_path.c_str());
  if (result == -1)
    return -errno;
  return result;
}


/**
 * Renames a file.  If using alien_cache on NFS, need to avoid deleting
 * an existing file that was created by more than one node, so use
 * link()/unlink() to rename, and ignore if the file already exists.
 *
 * @param[in] oldpath Absolute path of the file to rename
 * @param[in] newpath Absolute path to rename the file to
 * \return Zero on success, non-zero else.
 */

static int Rename(const char *oldpath, const char *newpath) {
  if (!alien_cache_on_nfs_) {
    return rename(oldpath, newpath);
  }

  int result = link(oldpath, newpath);
  if (result < 0) {
    if (errno == EEXIST)
      LogCvmfs(kLogCache, kLogDebug, "%s already existed, ignoring", newpath);
    else
      return(result);
  }
  return(unlink(oldpath));
}


/**
 * Commits a file download started with StartTransaction(), i.e. renames
 * the temporary file to its real content hash name.
 *
 * If the cache is managed (quota / lru), it also inserts the file into
 * the lru database.
 *
 * @param[in] final_path Absolute content hash path of the file in local cache
 * @param[in] temp_path Absolute path of the temporoary file in local cache
 * @param[in] cvmfs_path Path of the chunk as seen in cvmfs
 * @param[in] hash Content hash of the file
 * @param[in] size Decompressed size of the file
 * \return Zero on success, non-zero else.
 */
static int CommitTransaction(const string &final_path,
                             const string &temp_path,
                             const string &cvmfs_path,
                             const shash::Any &hash,
                             const bool volatile_content,
                             const uint64_t size)
{
  int result;
  LogCvmfs(kLogCache, kLogDebug, "commit %s %s",
           final_path.c_str(), temp_path.c_str());

  if (alien_cache_) {
    int retval = chmod(temp_path.c_str(), 0660);
    assert(retval == 0);
  }
  result = Rename(temp_path.c_str(), final_path.c_str());
  if (result < 0) {
    result = -errno;
    LogCvmfs(kLogCache, kLogDebug, "commit failed: %s", strerror(errno));
    unlink(temp_path.c_str());
  } else {
    // TODO-QUOTAMGR
    /*if (volatile_content) {
      quota::InsertVolatile(hash, size, cvmfs_path);
    } else {
      quota::Insert(hash, size, cvmfs_path);
    }*/
  }

  return result;
}


/**
 * Commits the memory blob buffer to the given chunk id and name on cvmfs.
 * No checking! The hash and the memory blob need to match.
 */
static bool CommitFromMem(const shash::Any &id, const unsigned char *buffer,
                          const uint64_t size, const std::string &cvmfs_path)
{
  string temp_path;
  string final_path;

  int fd = StartTransaction(id, &final_path, &temp_path);
  if (fd < 0)
    return false;

  ssize_t retval = write(fd, buffer, size);
  close(fd);
  if ((retval < 0) || (static_cast<uint64_t>(retval) != size)) {
    AbortTransaction(temp_path);
    return false;
  }

  const bool volatile_content = false;
  return CommitTransaction(final_path, temp_path, cvmfs_path, id,
                           volatile_content, size) == 0;
}


/**
 * Returns a read-only file descriptor for a specific catalog entry, which could
 * be a complete file in the CAS as well as a chunk of a file.
 * After successful call, the data resides in local cache.
 * File is downloaded via HTTP if it is not in the local cache.
 * If multiple concurrent requests arrive for a file, the requests are queued
 * and only the first one performs the download.
 *
 * @param[in] checksum     content hash of the file to be fetched
 * @param[in] size         the required disk size of the downloaded data chunk
 * @param[in] cvmfs_path   Path of the chunk as seen in cvmfs
 *
 * \return Read-only file descriptor for the file pointing into local cache.
 *         On failure a negative error code.
 */
static int Fetch(const shash::Any &checksum,
                 const uint64_t    size,
                 const string     &cvmfs_path,
                 const bool        volatile_content,
                 download::DownloadManager *download_manager)
{
  CallGuard call_guard;
  int fd_return;  // Read-only file descriptor that is returned
  int retval;

  // Try to open from local cache
  if ((fd_return = cache::Open(checksum)) >= 0) {
    LogCvmfs(kLogCache, kLogDebug, "hit: %s", cvmfs_path.c_str());

    // TODO-QUOTAMGR
    //if (cache_mode_ == kCacheReadWrite)
    //  quota::Touch(checksum);
    return fd_return;
  }

  if (cache_mode_ == kCacheReadOnly)
    return -EROFS;

  // TODO-QUOTAMGR
  /*if (size > quota::GetMaxFileSize()) {
    LogCvmfs(kLogCache, kLogDebug, "file too big for lru cache (%"PRIu64" "
                                   "requested but only %"PRIu64" bytes free)",
             size, quota::GetMaxFileSize());
    return -ENOSPC;
  }*/

  // Opportunitically clean up cache for large files
  // TODO-QUOTAMGR
  /*
  if ((size >= kBigFile) && (quota::GetCapacity() > 0)) {
    assert(quota::GetCapacity() >= size);
    quota::Cleanup(quota::GetCapacity() - size);
  }*/

  // Initialize TLS
  ThreadLocalStorage *tls = static_cast<ThreadLocalStorage *>(
                            pthread_getspecific(thread_local_storage_));
  if (tls == NULL) {
    tls = new ThreadLocalStorage();
    retval = pipe(tls->pipe_wait);
    assert(retval == 0);
    tls->download_job.destination = download::kDestinationFile;
    tls->download_job.compressed = true;
    tls->download_job.probe_hosts = true;
    retval = pthread_setspecific(thread_local_storage_, tls);
    assert(retval == 0);
    pthread_mutex_lock(&lock_tls_blocks_);
    tls_blocks_->push_back(tls);
    pthread_mutex_unlock(&lock_tls_blocks_);
  }

  // Lock queue and start downloading or enqueue
  pthread_mutex_lock(&lock_queues_download_);
  ThreadQueues::iterator iDownloadQueue = queues_download_->find(checksum);
  if (iDownloadQueue != queues_download_->end()) {
    LogCvmfs(kLogCache, kLogDebug, "waiting for download of %s",
             cvmfs_path.c_str());

    iDownloadQueue->second->push_back(tls->pipe_wait[1]);
    pthread_mutex_unlock(&lock_queues_download_);
    ReadPipe(tls->pipe_wait[0], &fd_return, sizeof(int));

    LogCvmfs(kLogCache, kLogDebug, "received from another thread fd %d for %s",
             fd_return, cvmfs_path.c_str());
    return fd_return;
  } else {
    // Seems we are the first one, check again in the cache (race condition)
    fd_return = cache::Open(checksum);
    if (fd_return >= 0) {
      pthread_mutex_unlock(&lock_queues_download_);
      // TODO-QUOTAMGR
      //quota::Touch(checksum);
      return fd_return;
    }

    // Create a new queue for this chunk
    (*queues_download_)[checksum] = &tls->other_pipes_waiting;
    pthread_mutex_unlock(&lock_queues_download_);
  }

  // The download path starts here
  LogCvmfs(kLogCache, kLogDebug, "downloading %s", cvmfs_path.c_str());
  atomic_inc64(&num_download_);

  const string url = "/data/" + checksum.MakePath();
  string final_path;
  string temp_path;
  int fd;  // Used to write the downloaded file
  FILE *f = NULL;
  int result = -EIO;

  fd = StartTransaction(checksum, &final_path, &temp_path);
  if (fd < 0) {
    LogCvmfs(kLogCache, kLogDebug, "could not start transaction on %s",
             final_path.c_str());
    result = fd;
    goto fetch_finalize;
  }

  f = fdopen(fd, "w");
  if (!f) {
    result = -errno;
    LogCvmfs(kLogCache, kLogDebug, "could not fdopen %s", final_path.c_str());
    goto fetch_finalize;
  }

  LogCvmfs(kLogCache, kLogDebug, "miss: %s %s",
           cvmfs_path.c_str(), url.c_str());

  tls->download_job.url = &url;
  tls->download_job.destination_file = f;
  tls->download_job.expected_hash = &checksum;
  tls->download_job.extra_info = &cvmfs_path;
  download_manager->Fetch(&tls->download_job);

  if (tls->download_job.error_code == download::kFailOk) {
    LogCvmfs(kLogCache, kLogDebug, "finished downloading of %s", url.c_str());

    // Check decompressed size (a cross check just in case)
    platform_stat64 stat_info;
    stat_info.st_size = -1;
    // allow size to be zero if alien cache, because hadoop-fuse-dfs
    //   returns size zero for a while
    if ((platform_fstat(fileno(f), &stat_info) != 0) ||
         ((stat_info.st_size != (int64_t)size) &&
         (!alien_cache_ || (stat_info.st_size != 0))))
    {
      LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
               "size check failure for %s, expected %lu, got %ld",
               url.c_str(), size, stat_info.st_size);
      if (!CopyPath2Path(temp_path, *cache_path_ + "/quarantaine/" +
                         checksum.ToString()))
      {
        LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
                 "failed to move %s to quarantaine", temp_path.c_str());
      }
      result = -EIO;
      goto fetch_finalize;
    }

    LogCvmfs(kLogCache, kLogDebug, "trying to commit %s", final_path.c_str());
    fclose(f);
    fd = -1;
    fd_return = ::open(temp_path.c_str(), O_RDONLY);
    if (fd_return < 0) {
      result = -errno;
      goto fetch_finalize;
    }
    result = cache::CommitTransaction(final_path, temp_path, cvmfs_path,
                                      checksum, volatile_content, size);
    if (result == 0) {
      platform_disable_kcache(fd_return);
      result = fd_return;
    } else {
      close(fd_return);
    }
  }

 fetch_finalize:
  // Cleanup
  LogCvmfs(kLogCache, kLogDebug, "finalizing download of %s",
           cvmfs_path.c_str());
  if (result < 0) {
    LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
             "failed to fetch %s (hash: %s, error %d)", cvmfs_path.c_str(),
             checksum.ToString().c_str(), tls->download_job.error_code);
  }
  if (fd >= 0) {
    if (f) {
      fclose(f);
    } else {
      close(fd);
    }
    AbortTransaction(temp_path);
  }

  // Signal the waiting threads and remove the queue
  pthread_mutex_lock(&lock_queues_download_);
  for (unsigned i = 0, s = tls->other_pipes_waiting.size(); i < s; ++i) {
    int fd_dup = (result >= 0) ? dup(result) : result;
    WritePipe(tls->other_pipes_waiting[i], &fd_dup, sizeof(int));
  }
  tls->other_pipes_waiting.clear();
  queues_download_->erase(checksum);
  pthread_mutex_unlock(&lock_queues_download_);

  return result;
}


/**
 * Returns a read-only file descriptor for a specific catalog entry.
 * After successful call, the file resides in local cache.
 *
 * @param[in] d           Demanded catalog entry
 * @param[in] cvmfs_path  Path of the chunk as seen in cvmfs
 * \return Read-only file descriptor for the file pointing into local cache.
 *         On failure a negative error code.
 */
int FetchDirent(const catalog::DirectoryEntry &d,
                const string &cvmfs_path,
                const bool volatile_content,
                download::DownloadManager *download_manager)
{
  return Fetch(d.checksum(),
               d.size(),
               cvmfs_path,
               volatile_content,
               download_manager);
}


/**
 * Returns a read-only file descriptor for a specific file chunk
 * After successful call, the file chunk resides in local cache.
 *
 * @param[in] chunk       Demanded file chunk
 * @param[in] cvmfs_path  Path of the full file as seen in cvmfs
 * \return Read-only file descriptor for the file pointing into local cache.
 *         On failure a negative error code.
 */
int FetchChunk(const FileChunk &chunk,
               const string &cvmfs_path,
               const bool volatile_content,
               download::DownloadManager *download_manager)
{
  return Fetch(chunk.content_hash(),
               chunk.size(),
               cvmfs_path,
               volatile_content,
               download_manager);
}


int64_t GetNumDownloads() {
  return atomic_read64(&num_download_);
}


CatalogManager::CatalogManager(const string &repo_name,
                               signature::SignatureManager *signature_manager,
                               download::DownloadManager *download_manager,
                               perf::Statistics *statistics)
                             : AbstractCatalogManager(statistics) {
  LogCvmfs(kLogCache, kLogDebug, "constructing cache catalog manager");
  repo_name_ = repo_name;
  signature_manager_ = signature_manager;
  download_manager_ = download_manager;
  offline_mode_ = false;
  loaded_inodes_ = all_inodes_ = 0;
  n_certificate_hits_ = statistics->Register("cache.n_certificate_hits",
      "Number of certificate hits");
  n_certificate_misses_ = statistics->Register("cache.n_certificate_misses",
      "Number of certificate misses");
}


/**
 * Specialized initialization that uses a fixed root hash.
 */
bool CatalogManager::InitFixed(const shash::Any &root_hash) {
  LogCvmfs(kLogCatalog, kLogDebug, "Initialize catalog with root hash %s",
           root_hash.ToString().c_str());
  WriteLock();
  bool attached = MountCatalog(PathString("", 0), root_hash, NULL);
  Unlock();

  if (!attached) {
    LogCvmfs(kLogCatalog, kLogDebug, "failed to initialize root catalog");
  }

  return attached;
}


catalog::Catalog *CatalogManager::CreateCatalog(
  const PathString  &mountpoint,
  const shash::Any  &catalog_hash,
  catalog::Catalog  *parent_catalog
) {
  mounted_catalogs_[mountpoint] = loaded_catalogs_[mountpoint];
  loaded_catalogs_.erase(mountpoint);
  return new catalog::Catalog(mountpoint, catalog_hash, parent_catalog);
}


/**
 * Triggered when the catalog is attached (db file opened)
 */
void CatalogManager::ActivateCatalog(catalog::Catalog *catalog) {
  const catalog::Counters &counters =
                    const_cast<const catalog::Catalog*>(catalog)->GetCounters();
  if (catalog->IsRoot()) {
    all_inodes_ = counters.GetAllEntries();
  }
  loaded_inodes_ += counters.GetSelfEntries();
}


catalog::LoadError CatalogManager::LoadCatalogCas(const shash::Any &hash,
                                                  const string &cvmfs_path,
                                                  std::string *catalog_path)
{
  assert(hash.suffix == shash::kSuffixCatalog);

  CallGuard call_guard;
  int64_t size;
  int retval;
  bool pin_retval;

  // Try from cache
  const string cache_path = *cache_path_ + "/" + hash.MakePathWithoutSuffix();
  if (alien_cache_) {
    *catalog_path = cache_path;
    if (FileExists(cache_path)) {
      // on alien cache, if the file exists, just use it
      LogCvmfs(kLogCache, kLogDebug, "found catalog %s in alien cache",
               hash.ToString().c_str());
      return catalog::kLoadNew;
    }
  } else {
    *catalog_path = cache_path + "T";
    retval = Rename(cache_path.c_str(), catalog_path->c_str());
    if (retval == 0) {
      LogCvmfs(kLogCache, kLogDebug, "found catalog %s in cache",
               hash.ToString().c_str());

      if (cache_mode_ == kCacheReadWrite) {
        size = GetFileSize(catalog_path->c_str());
        //TODO-QUOTAMGR
        /*
        pin_retval = quota::Pin(hash, uint64_t(size), cvmfs_path, true);
        if (!pin_retval) {
          quota::Remove(hash);
          unlink(catalog_path->c_str());
          LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
                   "failed to pin cached copy of catalog %s (no space)",
                   hash.ToString().c_str());
          return catalog::kLoadNoSpace;
        }
        */
      }
      // Pinned, can be safely renamed
      retval = Rename(catalog_path->c_str(), cache_path.c_str());
      *catalog_path = cache_path;
      return catalog::kLoadNew;
    }
  }

  if (cache_mode_ == kCacheReadOnly)
    return catalog::kLoadFail;

  // Download
  string temp_path;
  int catalog_fd = StartTransaction(hash, catalog_path, &temp_path);
  if (catalog_fd < 0)
    return catalog::kLoadFail;

  FILE *catalog_file = fdopen(catalog_fd, "w");
  if (!catalog_file) {
    AbortTransaction(temp_path);
    return catalog::kLoadFail;
  }

  const string url = "/data/" + hash.MakePath();
  download::JobInfo download_catalog(&url, true, true, catalog_file, &hash);
  download_catalog.extra_info = &cvmfs_path;
  download_manager_->Fetch(&download_catalog);
  fclose(catalog_file);
  if (download_catalog.error_code != download::kFailOk) {
    LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
             "unable to load catalog with key %s (%d - %s)",
             hash.ToString().c_str(), download_catalog.error_code,
             download::Code2Ascii(download_catalog.error_code));
    AbortTransaction(temp_path);
    backoff_throttle_.Throttle();
    return catalog::kLoadFail;
  }

  //TODO-QUOTAMGR
  size = GetFileSize(temp_path.c_str());
  /*
  if (uint64_t(size) > quota::GetMaxFileSize()) {
    LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
             "failed to load catalog %s (too big)",
             hash.ToString().c_str());
    AbortTransaction(temp_path);
    backoff_throttle_.Throttle();
    return catalog::kLoadNoSpace;
  }*/

  // Instead of commit, manually rename and pin, otherwise there is a race
  //TODO-QUOTAMGR
  pin_retval =0;
  /*
  pin_retval = quota::Pin(hash, uint64_t(size), cvmfs_path, true);
  if (!pin_retval) {
    LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
             "failed to pin catalog %s (no space)", hash.ToString().c_str());
    AbortTransaction(temp_path);
    backoff_throttle_.Throttle();
    return catalog::kLoadNoSpace;
  }*/

  retval = chmod(temp_path.c_str(), 0660);
  assert(retval == 0);
  retval = Rename(temp_path.c_str(), catalog_path->c_str());
  if (retval != 0) {
    //TODO-QUOTAMGR
    //quota::Remove(hash);
    backoff_throttle_.Throttle();
    return catalog::kLoadFail;
  }
  return catalog::kLoadNew;
}


catalog::LoadError CatalogManager::LoadCatalog(const PathString  &mountpoint,
                                               const shash::Any  &hash,
                                               std::string       *catalog_path,
                                               shash::Any        *catalog_hash)
{
  CallGuard call_guard;
  string cvmfs_path = "file catalog at " + repo_name_ + ":" +
    (mountpoint.IsEmpty() ?
      "/" : string(mountpoint.GetChars(), mountpoint.GetLength()));
  bool retval;

  // send the catalog hash to a blind memory position if it zero (save some ifs)
  shash::Any blind_hash;
  if (catalog_hash == NULL) {
    catalog_hash = &blind_hash;
  }

  // Load a particular catalog
  if (!hash.IsNull()) {
    cvmfs_path += " (" + hash.ToString() + ")";
    catalog::LoadError load_error = LoadCatalogCas(hash, cvmfs_path,
                                                   catalog_path);
    if (load_error == catalog::kLoadNew)
      loaded_catalogs_[mountpoint] = hash;
    *catalog_hash = hash;
    return load_error;
  }

  // Happens only on init/remount, i.e. quota won't delete a cached catalog
  string checksum_path = quota::checksum_file_prefix + "." + repo_name_;
  if (alien_cache_ && !FileExists(checksum_path)) {
    // In case the alien cache has been preloaded, the .cvmfschecksum file
    // must be read from the alien cache instead of the client cache
    checksum_path = (*cache_path_) + "/" + checksum_path;
  }
  shash::Any cache_hash;
  uint64_t cache_last_modified = 0;

  // Load local checksum
  FILE *file_checksum = fopen(checksum_path.c_str(), "r");
  char tmp[128];
  int read_bytes;
  if (file_checksum && (read_bytes = fread(tmp, 1, 128, file_checksum)) > 0) {
    // Separate hash from timestamp
    int separator_pos = 0;
    for (; (separator_pos < read_bytes) && (tmp[separator_pos] != 'T');
         ++separator_pos) { }
    cache_hash = shash::MkFromHexPtr(shash::HexPtr(string(tmp, separator_pos)),
                                     shash::kSuffixCatalog);
    if (!FileExists(*cache_path_ + "/" + cache_hash.MakePathWithoutSuffix())) {
      LogCvmfs(kLogCache, kLogDebug, "found checksum hint without catalog");
      cache_hash = shash::Any();
    } else {
      // Get local last modified time
      string str_modified;
      if ((tmp[separator_pos] == 'T') && (read_bytes > (separator_pos+1))) {
        str_modified = string(tmp+separator_pos+1,
                              read_bytes-(separator_pos+1));
        cache_last_modified = String2Uint64(str_modified);
        LogCvmfs(kLogCache, kLogDebug, "cached copy publish date %s",
                 StringifyTime(cache_last_modified, true).c_str());
      }
    }
  } else {
    LogCvmfs(kLogCache, kLogDebug, "unable to read local checksum");
  }
  if (file_checksum) fclose(file_checksum);

  // Load and verify remote checksum
  manifest::Failures manifest_failure;
  cache::ManifestEnsemble ensemble(this);
  manifest_failure = manifest::Fetch("", repo_name_, cache_last_modified,
                                     &cache_hash, signature_manager_,
                                     download_manager_,
                                     &ensemble);
  if (manifest_failure != manifest::kFailOk) {
    LogCvmfs(kLogCache, kLogDebug, "failed to fetch manifest (%d - %s)",
             manifest_failure, manifest::Code2Ascii(manifest_failure));

    if (!cache_hash.IsNull()) {
      if (catalog_path) {
        if (cache_mode_ == kCacheReadWrite) {
          *catalog_path =
            *cache_path_ + "/" + cache_hash.MakePathWithoutSuffix();
          //TODO-QUOTAMGR
          /*
          int64_t size = GetFileSize(*catalog_path);
          retval = quota::Pin(cache_hash, uint64_t(size),
                              cvmfs_path, true);
          if (!retval) {
            LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
                     "failed to pin cached root catalog (no space)");
            return catalog::kLoadFail;
          }*/
        }
      }
      loaded_catalogs_[mountpoint] = cache_hash;
      *catalog_hash = cache_hash;
      offline_mode_ = true;

      return catalog::kLoadUp2Date;
    }
    return catalog::kLoadFail;
  }

  offline_mode_ = false;
  cvmfs_path += " (" + ensemble.manifest->catalog_hash().ToString() + ")";
  LogCvmfs(kLogCache, kLogDebug, "remote checksum is %s",
           ensemble.manifest->catalog_hash().ToString().c_str());

  // Short way out, use cached copy
  if (ensemble.manifest->catalog_hash() == cache_hash) {
    if (catalog_path) {
      *catalog_path = *cache_path_ + "/" + cache_hash.MakePathWithoutSuffix();
      // quota::Pin is only effective on first load, afterwards it is a NOP
      if (cache_mode_ == kCacheReadWrite) {
        //TODO-QUOTAMGR
        /*
        int64_t size = GetFileSize(*catalog_path);
        retval = quota::Pin(cache_hash, uint64_t(size),
                            cvmfs_path, true);
        if (!retval) {
          LogCvmfs(kLogCache, kLogDebug | kLogSyslogErr,
                   "failed to pin cached root catalog (no space)");
          return catalog::kLoadNoSpace;
        }*/
      }
      loaded_catalogs_[mountpoint] = cache_hash;
      *catalog_hash = cache_hash;
      return catalog::kLoadUp2Date;
    } else {
      loaded_catalogs_[mountpoint] = cache_hash;
      *catalog_hash = cache_hash;
      return catalog::kLoadUp2Date;
    }
  }
  if (!catalog_path)
    return catalog::kLoadNew;

  if (cache_mode_ == kCacheReadOnly)
    return catalog::kLoadFail;

  // Load new catalog
  catalog::LoadError load_retval =
    LoadCatalogCas(ensemble.manifest->catalog_hash(), cvmfs_path, catalog_path);
  if (load_retval != catalog::kLoadNew)
    return load_retval;
  loaded_catalogs_[mountpoint] = ensemble.manifest->catalog_hash();
  *catalog_hash = ensemble.manifest->catalog_hash();

  // Store new manifest and certificate
  CommitFromMem(ensemble.manifest->certificate(),
                ensemble.cert_buf, ensemble.cert_size,
                "certificate for " + repo_name_);
  retval = ensemble.manifest->ExportChecksum(".", 0600);
  if (!retval) {
    unlink(checksum_path.c_str());
  }

  return catalog::kLoadNew;
}


void CatalogManager::UnloadCatalog(const catalog::Catalog *catalog) {
  LogCvmfs(kLogCache, kLogDebug, "unloading catalog %s",
           catalog->path().c_str());

  map<PathString, shash::Any>::iterator iter =
    mounted_catalogs_.find(catalog->path());
  assert(iter != mounted_catalogs_.end());

  // TODO-QUOTAMGR
  /*
  if (cache_mode_ == kCacheReadWrite)
    quota::Unpin(iter->second);
  */

  mounted_catalogs_.erase(iter);
  const catalog::Counters &counters = catalog->GetCounters();
  loaded_inodes_ -= counters.GetSelfEntries();
}


CatalogManager::~CatalogManager() {
  LogCvmfs(kLogCache, kLogDebug, "unpinning / unloading all catalogs");

  if (cache_mode_ == kCacheReadWrite) {
    for (map<PathString, shash::Any>::iterator i = mounted_catalogs_.begin(),
         iend = mounted_catalogs_.end(); i != iend; ++i)
    {
      //TODO-QUOTAMGR
      //quota::Unpin(i->second);
    }
  }
  mounted_catalogs_.clear();
}


void ManifestEnsemble::FetchCertificate(const shash::Any &hash) {
  uint64_t size;
  bool retval = Open2Mem(hash, &cert_buf, &size);
  cert_size = size;
  if (retval)
    perf::Inc(catalog_mgr_->n_certificate_hits_);
  else
    perf::Inc(catalog_mgr_->n_certificate_misses_);
}

}  // namespace cache
