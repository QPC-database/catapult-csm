// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "atrace_process_dump.h"

#include <stdint.h>

#include "file_utils.h"
#include "logging.h"
#include "procfs_utils.h"

AtraceProcessDump::AtraceProcessDump() {
  self_pid_ = static_cast<int>(getpid());
}

AtraceProcessDump::~AtraceProcessDump() {
}

void AtraceProcessDump::RunAndPrintJson(FILE* stream) {
  out_ = stream;

  fprintf(out_, "{\"start_ts\": \"%llu\", \"snapshots\":[\n",
      time_utils::GetTimestamp());

  CHECK(dump_timer_);
  dump_timer_->Start();
  for (int dump_number = 0; dump_number < dump_count_; dump_number++) {
    if (dump_number > 0) {
      if (!dump_timer_->Wait())
        break;  // Interrupted by signal.
      fprintf(out_, ",\n");
    }
    TakeGlobalSnapshot();
    SerializeSnapshot();
    fflush(out_);
  }
  fprintf(out_, "],\n");
  SerializePersistentProcessInfo();
  fprintf(out_, "}\n");
  fflush(out_);
  Cleanup();
}

void AtraceProcessDump::Stop() {
  CHECK(dump_timer_);
  dump_timer_->Stop();
}

void AtraceProcessDump::TakeGlobalSnapshot() {
  snapshot_.clear();
  snapshot_timestamp_ = time_utils::GetTimestamp();

  file_utils::ForEachPidInProcPath("/proc", [this](int pid) {
    // Skip if not regognized as a process.
    if (!UpdatePersistentProcessInfo(pid))
      return;
    const ProcessInfo* process = processes_[pid].get();
    // Snapshot can't be obtained for kernel workers.
    if (process->in_kernel)
      return;

    ProcessSnapshot* process_snapshot = new ProcessSnapshot();
    snapshot_[pid] = std::unique_ptr<ProcessSnapshot>(process_snapshot);

    process_snapshot->pid = pid;
    procfs_utils::ReadOomStats(process_snapshot);
    procfs_utils::ReadPageFaultsAndCpuTimeStats(process_snapshot);

    if (ShouldTakeFullDump(process)) {
      process_snapshot->memory.ReadFullStats(pid);
    } else {
      process_snapshot->memory.ReadLightStats(pid);
    }
    if (graphics_stats_ && process->is_app) {
      process_snapshot->memory.ReadGpuStats(pid);
    }
  });
}

bool AtraceProcessDump::UpdatePersistentProcessInfo(int pid) {
  if (!processes_.count(pid)) {
    if (procfs_utils::ReadTgid(pid) != pid)
      return false;
    processes_[pid] = procfs_utils::ReadProcessInfo(pid);
  }
  ProcessInfo* process = processes_[pid].get();
  procfs_utils::ReadProcessThreads(process);

  if (full_dump_mode_ == FullDumpMode::kOnlyWhitelisted &&
      full_dump_whitelist_.count(process->name)) {
    full_dump_whitelisted_pids_.insert(pid);
  }
  return true;
}

bool AtraceProcessDump::ShouldTakeFullDump(const ProcessInfo* process) {
  if (full_dump_mode_ == FullDumpMode::kAllProcesses)
    return !process->in_kernel && (process->pid != self_pid_);
  if (full_dump_mode_ == FullDumpMode::kAllJavaApps)
    return process->is_app;
  if (full_dump_mode_ == FullDumpMode::kDisabled)
    return false;
  return full_dump_whitelisted_pids_.count(process->pid) > 0;
}

void AtraceProcessDump::SerializeSnapshot() {
  fprintf(out_, "{\"ts\":\"%llu\",\"memdump\":{\n", snapshot_timestamp_);
  for (auto it = snapshot_.begin(); it != snapshot_.end();) {
    const ProcessSnapshot* process = it->second.get();
    const ProcessMemoryStats* mem = &process->memory;
    fprintf(out_, "\"%d\":{", process->pid);

    fprintf(out_, "\"vm\":%llu,\"rss\":%llu",
            mem->virt_kb(), mem->rss_kb());

    fprintf(out_, ",\"oom_sc\":%d,\"oom_sc_adj\":%d"
                  ",\"min_flt\":%lu,\"maj_flt\":%lu"
                  ",\"utime\":%lu,\"stime\":%lu",
            process->oom_score, process->oom_score_adj,
            process->minor_faults, process->major_faults,
            process->utime, process->stime);

    if (mem->full_stats_available()) {
      fprintf(out_, ",\"pss\":%llu,\"swp\":%llu"
                    ",\"pc\":%llu,\"pd\":%llu,\"sc\":%llu,\"sd\":%llu",
              mem->pss_kb(), mem->swapped_kb(),
              mem->private_clean_kb(), mem->private_dirty_kb(),
              mem->shared_clean_kb(), mem->shared_dirty_kb());
    }

    if (mem->gpu_stats_available()) {
      fprintf(out_, ",\"gpu_egl\":%llu,\"gpu_egl_pss\":%llu"
                    ",\"gpu_gl\":%llu,\"gpu_gl_pss\":%llu"
                    ",\"gpu_etc\":%llu,\"gpu_etc_pss\":%llu",
              mem->gpu_graphics_kb(), mem->gpu_graphics_pss_kb(),
              mem->gpu_gl_kb(), mem->gpu_gl_pss_kb(),
              mem->gpu_other_kb(), mem->gpu_other_pss_kb());
    }

    // Memory maps are too heavy to serialize. Enable only in whitelisting mode.
    if (print_smaps_ &&
        full_dump_mode_ == FullDumpMode::kOnlyWhitelisted &&
        mem->full_stats_available() &&
        full_dump_whitelisted_pids_.count(process->pid)) {

      fprintf(out_, ", \"mmaps\":[");
      size_t n_mmaps = mem->mmaps_count();
      for (size_t k = 0; k < n_mmaps; ++k) {
        const ProcessMemoryStats::MmapInfo* mm = mem->mmap(k);
        fprintf(out_,
                "{\"vm\":\"%llx-%llx\",\"file\":\"%s\",\"flags\":\"%s\","
                "\"pss\":%llu,\"rss\":%llu,\"swp\":%llu,"
                "\"pc\":%llu,\"pd\":%llu,"
                "\"sc\":%llu,\"sd\":%llu}",
                mm->start_addr, mm->end_addr, mm->mapped_file, mm->prot_flags,
                mm->pss_kb, mm->rss_kb, mm->swapped_kb,
                mm->private_clean_kb, mm->private_dirty_kb,
                mm->shared_clean_kb, mm->shared_dirty_kb);
        if (k < n_mmaps - 1)
          fprintf(out_, ", ");
      }
      fprintf(out_, "]");
    }

    if (++it != snapshot_.end())
      fprintf(out_, "},\n");
    else
      fprintf(out_, "}}\n");
  }
  fprintf(out_, "}");
}

void AtraceProcessDump::SerializePersistentProcessInfo() {
  fprintf(out_, "\"processes\":{");
  for (auto it = processes_.begin(); it != processes_.end();) {
    const ProcessInfo* process = it->second.get();
    fprintf(out_, "\"%d\":{", process->pid);
    fprintf(out_, "\"name\":\"%s\"", process->name);

    if (!process->in_kernel) {
      fprintf(out_, ",\"exe\":\"%s\",", process->exe);
      fprintf(out_, "\"threads\":{\n");
      const auto threads = &process->threads;
      for (auto thread_it = threads->begin(); thread_it != threads->end();) {
        const ThreadInfo* thread = &(thread_it->second);
        fprintf(out_, "\"%d\":{", thread->tid);
        fprintf(out_, "\"name\":\"%s\"", thread->name);

        if (++thread_it != threads->end())
          fprintf(out_, "},\n");
        else
          fprintf(out_, "}\n");
      }
      fprintf(out_, "}");
    }

    if (++it != processes_.end())
      fprintf(out_, "},\n");
    else
      fprintf(out_, "}\n");
  }
  fprintf(out_, "}");
}

void AtraceProcessDump::Cleanup() {
  processes_.clear();
  snapshot_.clear();
  full_dump_whitelisted_pids_.clear();
  dump_timer_ = nullptr;
}
