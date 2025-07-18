// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "common/be_mock_util.h"
#include "common/status.h"
#include "util/threadpool.h"

namespace doris {
class ExecEnv;

namespace vectorized {
class Scanner;
} // namespace vectorized

template <typename T>
class BlockingQueue;
} // namespace doris

namespace doris::vectorized {
class ScannerDelegate;
class ScanTask;
class ScannerContext;
class SimplifiedScanScheduler;

// Responsible for the scheduling and execution of all Scanners of a BE node.
// Execution thread pool
//     When a ScannerContext is launched, it will submit the running scanners to this scheduler.
//     The scheduling thread will submit the running scanner and its ScannerContext
//     to the execution thread pool to do the actual scan task.
//     Each Scanner will act as a producer, read the next block and put it into
//     the corresponding block queue.
//     The corresponding ScanNode will act as a consumer to consume blocks from the block queue.
//     After the block is consumed, the unfinished scanner will resubmit to this scheduler.
class ScannerScheduler {
public:
    ScannerScheduler();
    virtual ~ScannerScheduler();

    [[nodiscard]] Status init(ExecEnv* env);

    MOCK_FUNCTION Status submit(std::shared_ptr<ScannerContext> ctx,
                                std::shared_ptr<ScanTask> scan_task);

    void stop();

    int remote_thread_pool_max_thread_num() const { return _remote_thread_pool_max_thread_num; }

    static int get_remote_scan_thread_num();

    static int get_remote_scan_thread_queue_size();

private:
    static void _scanner_scan(std::shared_ptr<ScannerContext> ctx,
                              std::shared_ptr<ScanTask> scan_task);

    // true is the scheduler is closed.
    std::atomic_bool _is_closed = {false};
    bool _is_init = false;
    int _remote_thread_pool_max_thread_num;
};

struct SimplifiedScanTask {
    SimplifiedScanTask() = default;
    SimplifiedScanTask(std::function<void()> scan_func,
                       std::shared_ptr<vectorized::ScannerContext> scanner_context) {
        this->scan_func = scan_func;
        this->scanner_context = scanner_context;
    }

    std::function<void()> scan_func;
    std::shared_ptr<vectorized::ScannerContext> scanner_context = nullptr;
};

class SimplifiedScanScheduler {
public:
    SimplifiedScanScheduler(std::string sched_name, std::shared_ptr<CgroupCpuCtl> cgroup_cpu_ctl,
                            std::string workload_group = "system")
            : _is_stop(false),
              _cgroup_cpu_ctl(cgroup_cpu_ctl),
              _sched_name(sched_name),
              _workload_group(workload_group) {}

    MOCK_FUNCTION ~SimplifiedScanScheduler() {
#ifndef BE_TEST
        stop();
#endif
        LOG(INFO) << "Scanner sche " << _sched_name << " shutdown";
    }

    void stop() {
        _is_stop.store(true);
        _scan_thread_pool->shutdown();
        _scan_thread_pool->wait();
    }

    Status start(int max_thread_num, int min_thread_num, int queue_size) {
        RETURN_IF_ERROR(ThreadPoolBuilder(_sched_name, _workload_group)
                                .set_min_threads(min_thread_num)
                                .set_max_threads(max_thread_num)
                                .set_max_queue_size(queue_size)
                                .set_cgroup_cpu_ctl(_cgroup_cpu_ctl)
                                .build(&_scan_thread_pool));
        return Status::OK();
    }

    Status submit_scan_task(SimplifiedScanTask scan_task) {
        if (!_is_stop) {
            return _scan_thread_pool->submit_func([scan_task] { scan_task.scan_func(); });
        } else {
            return Status::InternalError<false>("scanner pool {} is shutdown.", _sched_name);
        }
    }

    void reset_thread_num(int new_max_thread_num, int new_min_thread_num) {
        int cur_max_thread_num = _scan_thread_pool->max_threads();
        int cur_min_thread_num = _scan_thread_pool->min_threads();
        if (cur_max_thread_num == new_max_thread_num && cur_min_thread_num == new_min_thread_num) {
            return;
        }
        if (new_max_thread_num >= cur_max_thread_num) {
            Status st_max = _scan_thread_pool->set_max_threads(new_max_thread_num);
            if (!st_max.ok()) {
                LOG(WARNING) << "Failed to set max threads for scan thread pool: "
                             << st_max.to_string();
            }
            Status st_min = _scan_thread_pool->set_min_threads(new_min_thread_num);
            if (!st_min.ok()) {
                LOG(WARNING) << "Failed to set min threads for scan thread pool: "
                             << st_min.to_string();
            }
        } else {
            Status st_min = _scan_thread_pool->set_min_threads(new_min_thread_num);
            if (!st_min.ok()) {
                LOG(WARNING) << "Failed to set min threads for scan thread pool: "
                             << st_min.to_string();
            }
            Status st_max = _scan_thread_pool->set_max_threads(new_max_thread_num);
            if (!st_max.ok()) {
                LOG(WARNING) << "Failed to set max threads for scan thread pool: "
                             << st_max.to_string();
            }
        }
    }

    void reset_max_thread_num(int thread_num) {
        int max_thread_num = _scan_thread_pool->max_threads();

        if (max_thread_num != thread_num) {
            Status st = _scan_thread_pool->set_max_threads(thread_num);
            if (!st.ok()) {
                LOG(INFO) << "reset max thread num failed, sche name=" << _sched_name;
            }
        }
    }

    void reset_min_thread_num(int thread_num) {
        int min_thread_num = _scan_thread_pool->min_threads();

        if (min_thread_num != thread_num) {
            Status st = _scan_thread_pool->set_min_threads(thread_num);
            if (!st.ok()) {
                LOG(INFO) << "reset min thread num failed, sche name=" << _sched_name;
            }
        }
    }

    MOCK_FUNCTION int get_queue_size() { return _scan_thread_pool->get_queue_size(); }

    MOCK_FUNCTION int get_active_threads() { return _scan_thread_pool->num_active_threads(); }

    int get_max_threads() { return _scan_thread_pool->max_threads(); }

    std::vector<int> thread_debug_info() { return _scan_thread_pool->debug_info(); }

    MOCK_FUNCTION Status schedule_scan_task(std::shared_ptr<ScannerContext> scanner_ctx,
                                            std::shared_ptr<ScanTask> current_scan_task,
                                            std::unique_lock<std::mutex>& transfer_lock);

private:
    std::unique_ptr<ThreadPool> _scan_thread_pool;
    std::atomic<bool> _is_stop;
    std::weak_ptr<CgroupCpuCtl> _cgroup_cpu_ctl;
    std::string _sched_name;
    std::string _workload_group;
    std::shared_mutex _lock;
};

} // namespace doris::vectorized
