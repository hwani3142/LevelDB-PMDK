// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <deque>
#include <limits>
#include <set>
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/posix_logger.h"
#include "util/env_posix_test_helper.h"

// JH
#include <iostream>
#include <fstream>
#include "pmem/pmem_directory.h"
#include "libpmemobj++/mutex.hpp"
// #include "pmem/pmem_file.h"
// #include "libpmemobj++/make_persistent_atomic.hpp"
#include "env_posix.cc"

#define POOLID "file"
#define POOL_DIR_ID "directory"

// HAVE_FDATASYNC is defined in the auto-generated port_config.h, which is
// included by port_stdcxx.h.
#if !HAVE_FDATASYNC
#define fdatasync fsync
#endif  // !HAVE_FDATASYNC

namespace leveldb {

namespace {

inline bool
file_exists (const std::string &name)
{
	std::ifstream f (name.c_str ());
	return f.good ();
}

// [pmem] JH
class PmemSequentialFile : public SequentialFile {
 private:
  std::string filename_;
  pobj::pool<rootFile> pool;
  pobj::persistent_ptr<rootFile> ptr;
  // int fd_;

 public:
  PmemSequentialFile(const std::string& fname, pobj::pool<rootFile> pool)
      : filename_(fname), pool(pool) { 
        ptr = pool.get_root();
  }


  virtual ~PmemSequentialFile() {
    pool.close();
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    // std::cout<<"Read \n";
    Status s;
    ssize_t r = ptr->file->Read(n, scratch);
    // std::cout<<"Read End\n";
    *result = Slice(scratch, r);
    // printf("Read%d '%s'\n", result->size(), result->data());
    return s;
  }

  virtual Status Skip(uint64_t n) {
    // std::cout<<"Skip \n";
    Status s = ptr->file->Skip(n);
    return s;
  }
};

// [pmem] JH
class PmemRandomAccessFile : public RandomAccessFile {
 private:
  std::string filename_;
  pobj::pool<rootFile> pool;
  pobj::persistent_ptr<rootFile> ptr;

 public:
  PmemRandomAccessFile(const std::string& fname, pobj::pool<rootFile> pool)
      : filename_(fname), pool(pool) { 
        ptr = pool.get_root();
  }

  virtual ~PmemRandomAccessFile() {
    pool.close();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    // std::cout<<"Read \n";
    Status s;
    ssize_t r = ptr->file->Read(offset, n, scratch);
    *result = Slice(scratch, (r<0 ? 0 : r));
    // std::cout<<"Read End\n";
    return s;
  }
};

// [pmem] JH
class PmemWritableFile : public WritableFile {
 private:
  std::string filename_;
  pobj::pool<rootFile> pool;
  pobj::persistent_ptr<rootFile> ptr;

  char buf_[kBufSize];
  size_t pos_;

 public:
  PmemWritableFile(const std::string& fname, pobj::pool<rootFile> pool)
      : filename_(fname), pool(pool), pos_(0) { 
        ptr = pool.get_root();
        pobj::transaction::exec_tx(pool, [&] {
          ptr->file = pobj::make_persistent<PmemFile> (pool);
        });
  }

  // PmemWritableFile(const std::string& fname)
  //     : filename_(fname) {
  //       if (!file_exists(filename_)) {
  //         // std::cout<<"not exist"<<std::endl;
  //         pool = pobj::pool<rootFile>::create (fname, POOLID,
  //                     // PMEMOBJ_MIN_POOL, S_IRUSR | S_IWUSR);
  //                     // ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR);
  //                     ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
  //       } else {
  //         // std::cout<<"exist"<<std::endl;
  //         pool = pobj::pool<rootFile>::open (fname, POOLID);
  //       } 
  //     }

  virtual ~PmemWritableFile() { }

  virtual Status Append(const Slice& data) {
    // size_t n = data.size();
    // const char* p = data.data();
    // pobj::transaction::exec_tx(pool, [&] {
    //   ptr->file = pobj::make_persistent<PmemFile> ();
    // });
    // printf("Append %d %s \n", data.size(), data.data());
    size_t n = data.size();
    const char* p = data.data();

    // Fit as much as possible into buffer.
    size_t copy = std::min(n, kBufSize - pos_); // data size OR remaining buffer size
    memcpy(buf_ + pos_, p, copy);
    p += copy;
    n -= copy;
    pos_ += copy;

    const uint32_t a = static_cast<uint32_t>(buf_[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(buf_[5]) & 0xff;
    const unsigned int type = buf_[6];
    const uint32_t length = a | (b << 8);
    // printf("%d %d, '%d'\n", copy, pos_, type);
    // printf("a: '%d' \n b: '%d' \n type: '%d'\n length: '%d'\n", a, b, type, length);

    if (n == 0) {
      return Status::OK();
    }

    // Can't fit in buffer, so need to do at least one write.
    // Current buffer -> flush
    // allocate new buffer (pos_ = 0)
    Status s = FlushBuffered();
    if (!s.ok()) {
      return s;
    }

    // Small writes go to buffer, large writes are written directly.
    if (n < kBufSize) {
      memcpy(buf_, p, n);
      pos_ = n;
      return Status::OK();
    }
    // Without through buffer, write.
    return WriteRaw(p, n);

    // std::cout<< "After append "<< ptr->file->getContentsSize()<<"\n";
  }

  virtual Status Close() {
    Status result = FlushBuffered();
    pool.close();
    return result;
  }

  virtual Status Flush() {
    return FlushBuffered();
  }

  pobj::persistent_ptr<rootFile> getFilePtr() {
    return pool.get_root();
  };

  Status SyncDirIfManifest() {
    // const char* f = filename_.c_str();
    // const char* sep = strrchr(f, '/');
    // Slice basename;
    // std::string dir;
    // if (sep == nullptr) {
    //   dir = ".";
    //   basename = f;
    // } else {
    //   dir = std::string(f, sep - f);
    //   basename = sep + 1;
    // }
    // Status s;
    // if (basename.starts_with("MANIFEST")) {
    //   int fd = open(dir.c_str(), O_RDONLY);
    //   if (fd < 0) {
    //     s = PosixError(dir, errno);
    //   } else {
    //     if (fsync(fd) < 0) {
    //       s = PosixError(dir, errno);
    //     }
    //     close(fd);
    //   }
    // }
    // return s;
    return Status::OK();
  }

  virtual Status Sync() {
    // printf("Sync\n");
    // Ensure new files referred to by the manifest are in the filesystem.
    Status s = SyncDirIfManifest();
    // if (!s.ok()) {
    //   return s;
    // }
    s = FlushBuffered();
    // if (s.ok()) {
    //   if (fdatasync(fd_) != 0) {
    //     s = PosixError(filename_, errno);
    //   }
    // }
    return s;
    // return Status::OK();
  }

 private:
  Status FlushBuffered() {
    Status s = WriteRaw(buf_, pos_);
    pos_ = 0;
    return s;
    // return Status::OK();
  }

  Status WriteRaw(const char* p, size_t n) {
    while (n > 0) {
      Slice data = Slice(p, n);
      ssize_t r = ptr->file->Append(p, n);
      // ssize_t r = ptr->file->Append(data);      
      // printf("WriteRaw '%c' '%c' '%c' '%c' '%c' '%c' '%c'\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
      // printf("WriteRaw %d %s %d\n", n, p, r);
      // char buf;
      // memcpy(&buf, p+6, sizeof(char));
      // printf("WriteRaw %d '%d'\n", n, p[6]);
      // printf("WriteRaw %d %s %d\n", data.size(), data.data(), r);
      // ssize_t r = write(fd_, p, n);
      if (r < 0) {
        if (errno == EINTR) {
          continue;  // Retry
        }
        return PosixError(filename_, errno);
      }
      p += r;
      n -= r;
    }
    return Status::OK();
  }
};


// class PosixFileLock : public FileLock {
//  public:
//   int fd_;
//   std::string name_;
// };

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
// class PosixLockTable {
//  private:
//   port::Mutex mu_;
//   std::set<std::string> locked_files_ GUARDED_BY(mu_);
//  public:
//   bool Insert(const std::string& fname) LOCKS_EXCLUDED(mu_) {
//     MutexLock l(&mu_);
//     return locked_files_.insert(fname).second;
//   }
//   void Remove(const std::string& fname) LOCKS_EXCLUDED(mu_) {
//     MutexLock l(&mu_);
//     locked_files_.erase(fname);
//   }
// };

class PmemEnv : public Env {
 public:
  PmemEnv();
  virtual ~PmemEnv() {
    char msg[] = "Destroying Env::Default()\n";
    fwrite(msg, 1, sizeof(msg), stderr);

    // JH
    pobj::delete_persistent<rootDirectory>(Dir_ptr);
    Dir_pool.close();

    abort();

  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    // std::cout<< "NewSequentialFile "<<fname<<" \n";
    Status s;
    pobj::pool<rootFile> pool;
    if (!file_exists(fname)) {
      pool = pobj::pool<rootFile>::create (fname, POOLID,
          ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    } else {
      pool = pobj::pool<rootFile>::open (fname, POOLID);
    } 
    // *result = new PmemWritableFile(fname);
    *result = new PmemSequentialFile(fname, pool);
    
    // std::cout<< "Center2 \n";
    // Status a = Dir_ptr->dir->Append(&Dir_pool, pool.get_root());
    // Status a = Dir_ptr->dir->Append(&pool);
    // std::cout<< "Finish Dir append "<<" \n";
    // pobj::persistent_ptr<rootFile> ptr = pool.get_root();
    // std::cout<< "Finish Dir append "<<" \n";
    // std::cout<< "Finish Dir append "<< ptr->file->getContentsSize()<<" \n";

    return s;

  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) {
    // std::cout<< "NewRandomAccessFile "<<fname<<" \n";
    Status s;
    pobj::pool<rootFile> pool;
    if (!file_exists(fname)) {
      pool = pobj::pool<rootFile>::create (fname, POOLID,
          ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    } else {
      pool = pobj::pool<rootFile>::open (fname, POOLID);
    } 
    *result = new PmemRandomAccessFile(fname, pool);
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    Status s;
    // std::cout<< "NewWritableFile "<<fname<<" \n";
    pobj::pool<rootFile> pool;
    if (!file_exists(fname)) {
      pool = pobj::pool<rootFile>::create (fname, POOLID,
          ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    } else {
      pool = pobj::pool<rootFile>::open (fname, POOLID);
    } 
    // *result = new PmemWritableFile(fname);
    *result = new PmemWritableFile(fname, pool);
    
    // std::cout<< "Center2 \n";
    Status a = Dir_ptr->dir->Append(&Dir_pool, pool.get_root());
    // Status a = Dir_ptr->dir->Append(&pool);
    // std::cout<< "Finish Dir append "<<" \n";
    // pobj::persistent_ptr<rootFile> ptr = pool.get_root();
    // std::cout<< "Finish Dir append "<<" \n";
    // std::cout<< "Finish Dir append "<< ptr->file->getContentsSize()<<" \n";

    return s;
  }

  virtual Status NewAppendableFile(const std::string& fname,
                                   WritableFile** result) {
    Status s;
    // std::cout<< "NewAppendableFile \n";
    pobj::pool<rootFile> pool;
    if (!file_exists(fname)) {
      pool = pobj::pool<rootFile>::create (fname, POOLID,
          ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    } else {
      pool = pobj::pool<rootFile>::open (fname, POOLID);
    } 
    // *result = new PmemWritableFile(fname);
    *result = new PmemWritableFile(fname, pool);
    
    // std::cout<< "Center2 \n";
    Status a = Dir_ptr->dir->Append(&Dir_pool, pool.get_root());
    // Status a = Dir_ptr->dir->Append(&pool);
    // std::cout<< "Finish Dir append "<<" \n";
    // pobj::persistent_ptr<rootFile> ptr = pool.get_root();
    // std::cout<< "Finish Dir append "<<" \n";
    // std::cout<< "Finish Dir append "<< ptr->file->getContentsSize()<<" \n";

    return s;
  }

  virtual bool FileExists(const std::string& fname) {
    return access(fname.c_str(), F_OK) == 0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
      return PosixError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = PosixError(fname, errno);
    }
    return result;
  }

  virtual Status CreateDir(const std::string& name) {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = PosixError(name, errno);
    }
    return result;
  }

  virtual Status DeleteDir(const std::string& name) {
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = PosixError(name, errno);
    }
    return result;
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = PosixError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = PosixError(src, errno);
    }
    return result;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = nullptr;
    Status result;
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = PosixError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock(fd, true) == -1) {
      result = PosixError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->fd_, false) == -1) {
      result = PosixError("unlock", errno);
    }
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
    FILE* f = fopen(fname.c_str(), "w");
    if (f == nullptr) {
      *result = nullptr;
      return PosixError(fname, errno);
    } else {
      *result = new PosixLogger(f, &PmemEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

 private:
  void PthreadCall(const char* label, int result) {
    if (result != 0) {
      fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
      abort();
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PmemEnv*>(arg)->BGThread();
    return nullptr;
  }

  pthread_mutex_t mu_;
  pthread_cond_t bgsignal_;
  pthread_t bgthread_;
  bool started_bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  PosixLockTable locks_;
  Limiter mmap_limit_;
  Limiter fd_limit_;

  // [Pmem] JH
  std::string path;
  pobj::pool<rootDirectory> Dir_pool;
  pobj::persistent_ptr<rootDirectory> Dir_ptr;
  // pobj::mutex mutex;

};

// // Return the maximum number of concurrent mmaps.
// static int MaxMmaps() {
//   if (mmap_limit >= 0) {
//     return mmap_limit;
//   }
//   // Up to 1000 mmaps for 64-bit binaries; none for smaller pointer sizes.
//   mmap_limit = sizeof(void*) >= 8 ? 1000 : 0;      
//   // std::cout << "mmap_limit_ : " << mmap_limit << std::endl;

//   return mmap_limit;
// }

// // Return the maximum number of read-only files to keep open.
// static intptr_t MaxOpenFiles() {
//   if (open_read_only_file_limit >= 0) {
//     return open_read_only_file_limit;
//   }
//   struct rlimit rlim;
//   if (getrlimit(RLIMIT_NOFILE, &rlim)) {
//     // getrlimit failed, fallback to hard-coded default.
//     open_read_only_file_limit = 50;
//   } else if (rlim.rlim_cur == RLIM_INFINITY) {
//     open_read_only_file_limit = std::numeric_limits<int>::max();
//   } else {
//     // Allow use of 20% of available file descriptors for read-only files.
//     open_read_only_file_limit = rlim.rlim_cur / 5;
//   }
//   // std::cout << "fd_limit_ : " << open_read_only_file_limit << std::endl;
//   return open_read_only_file_limit;
// }

PmemEnv::PmemEnv()
    : started_bgthread_(false),
      mmap_limit_(MaxMmaps()),
      fd_limit_(MaxOpenFiles()),
      path("/home/hwan/pmem_dir/Directory") {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, nullptr));
  PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, nullptr));
  // JH
  if (!FileExists(path)) {
    Dir_pool = pobj::pool<rootDirectory>::create(path, POOL_DIR_ID,
                  ((size_t)(1024 * 1024 * 64)), S_IRUSR | S_IWUSR);

  } else {
    Dir_pool = pobj::pool<rootDirectory>::open(path, POOL_DIR_ID);
  }
  Dir_ptr = Dir_pool.get_root();
  pobj::transaction::exec_tx(Dir_pool, [&] {
    Dir_ptr->dir = pobj::make_persistent<PmemDirectory> ();
  });

}

void PmemEnv::Schedule(void (*function)(void*), void* arg) {
  PthreadCall("lock", pthread_mutex_lock(&mu_));

  // Start background thread if necessary
  if (!started_bgthread_) {
    started_bgthread_ = true;
    PthreadCall(
        "create thread",
        pthread_create(&bgthread_, nullptr,  &PmemEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  if (queue_.empty()) {
    PthreadCall("signal", pthread_cond_signal(&bgsignal_));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

void PmemEnv::BGThread() {
  while (true) {
    // Wait until there is an item that is ready to run
    PthreadCall("lock", pthread_mutex_lock(&mu_));
    while (queue_.empty()) {
      PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
    }

    void (*function)(void*) = queue_.front().function;
    void* arg = queue_.front().arg;
    queue_.pop_front();

    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    (*function)(arg);
  }
}

// namespace {
// struct StartThreadState {
//   void (*user_function)(void*);
//   void* arg;
// };
// }
// static void* StartThreadWrapper(void* arg) {
//   StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
//   state->user_function(state->arg);
//   delete state;
//   return nullptr;
// }

void PmemEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, nullptr,  &StartThreadWrapper, state));
}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PmemEnv; }
// static void InitDefaultEnv() { default_env = new PosixEnv; }

void EnvPosixTestHelper::SetReadOnlyFDLimit(int limit) {
  assert(default_env == nullptr);
  open_read_only_file_limit = limit;
}

void EnvPosixTestHelper::SetReadOnlyMMapLimit(int limit) {
  assert(default_env == nullptr);
  mmap_limit = limit;
}

Env* Env::Default() {
  pthread_once(&once, InitDefaultEnv);
  return default_env;
}

}  // namespace leveldb