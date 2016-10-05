/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/util/common.h"
#include "scanner/util/jsoncpp.h"
#include "scanner/util/storehouse.h"
#include "scanner/util/util.h"
#include "storehouse/storage_backend.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h> /* PATH_MAX */
#include <string.h>
#include <sys/stat.h> /* mkdir(2) */
#include <cassert>
#include <cstdarg>
#include <sstream>
#include <iostream>

using storehouse::WriteFile;
using storehouse::RandomReadFile;
using storehouse::StoreResult;

namespace scanner {

int PUS_PER_NODE = 1;           // Number of available GPUs per node
int WORK_ITEM_SIZE = 8;         // Base size of a work item
int TASKS_IN_QUEUE_PER_PU = 4;  // How many tasks per GPU to allocate to a node
int LOAD_WORKERS_PER_NODE = 2;  // Number of worker threads loading data
int SAVE_WORKERS_PER_NODE = 2;  // Number of worker threads loading data
int NUM_CUDA_STREAMS = 32;      // Number of cuda streams for image processing

bool DatabaseMetadata::has_dataset(const std::string& dataset) {
  for (const auto& kv : dataset_names) {
    if (kv.second == dataset) {
      return true;
    }
  }
  return false;
}

bool DatabaseMetadata::has_dataset(i32 dataset_id) {
  return dataset_names.count(dataset_id) > 0;
}

i32 DatabaseMetadata::get_dataset_id(const std::string &dataset) {
  i32 id = -1;
  for (const auto& kv : dataset_names) {
    if (kv.second == dataset) {
      id = kv.first;
      break;
    }
  }
  assert(id != -1);
  return id;
}

const std::string& DatabaseMetadata::get_dataset_name(i32 dataset_id) {
  return dataset_names.at(dataset_id);
}

void DatabaseMetadata::add_dataset(const std::string &dataset) {
  i32 dataset_id = next_dataset_id++;
  dataset_names[dataset_id] = dataset;
  dataset_job_ids[dataset_id] = {};
}

void DatabaseMetadata::remove_dataset(i32 dataset_id) {
  for (i32 job_id : dataset_job_ids.at(dataset_id)) {
    job_names.erase(job_id);
  }
  dataset_job_ids.erase(dataset_id);
  dataset_names.erase(dataset_id);
}

bool DatabaseMetadata::has_job(const std::string &job) {
  for (const auto& kv : job_names) {
    if (job == kv.second) {
      return true;
    }
  }
  return false;
}

bool DatabaseMetadata::has_job(i32 job_id) {
  return job_names.count(job_id) > 0;
}

i32 DatabaseMetadata::get_job_id(const std::string &job) {
  i32 job_id = -1;
  for (const auto& kv : job_names) {
    if (job == kv.second) {
      job_id = kv.first;
      break;
    }
  }
  assert(job_id != -1);
  return job_id;
}

const std::string &DatabaseMetadata::get_job_name(i32 job_id) {
  return job_names.at(job_id);
}

void DatabaseMetadata::add_job(i32 dataset_id, const std::string &job_name) {
  i32 job_id = next_job_id++;
  dataset_job_ids.at(dataset_id).insert(job_id);
  job_names[job_id] = job_name;
}

void DatabaseMetadata::remove_job(i32 job_id) {
  for (auto& kv : dataset_job_ids) {
    if (kv.second.count(job_id) > 0) {
      kv.second.erase(job_id);
    }
  }
}

void serialize_database_metadata(storehouse::WriteFile* file,
                                 const DatabaseMetadata& metadata) {
  assert(metadata.dataset_names.size() == metadata.dataset_job_ids.size());

  write(file, metadata.next_dataset_id);
  write(file, metadata.next_job_id);

  size_t num_datasets = metadata.dataset_names.size();
  write(file, num_datasets);
  for (const auto& kv : metadata.dataset_names) {
    write(file, kv.first);
    write(file, kv.second);
  }
  for (const auto& kv : metadata.dataset_job_ids) {
    write(file, kv.first);
    size_t num_job_ids = kv.second.size();
    write(file, num_job_ids);
    for (i32 job_id : kv.second) {
      write(file, job_id);
    }
  }
  size_t num_jobs = metadata.job_names.size();
  write(file, num_jobs);
  for (const auto& kv : metadata.job_names) {
    write(file, kv.first);
    write(file, kv.second);
  }
}

DatabaseMetadata deserialize_database_metadata(storehouse::RandomReadFile* file,
                                               u64& pos) {
  DatabaseMetadata meta;
  meta.next_dataset_id = read<i32>(file, pos);
  meta.next_job_id = read<i32>(file, pos);

  size_t num_datasets = read<size_t>(file, pos);
  for (size_t i = 0; i < num_datasets; ++i) {
    i32 dataset_id = read<i32>(file, pos);
    meta.dataset_names[dataset_id] = read<std::string>(file, pos);
  }
  for (size_t i = 0; i < num_datasets; ++i) {
    i32 dataset_id = read<i32>(file, pos);
    size_t num_job_ids = read<size_t>(file, pos);
    meta.dataset_job_ids[dataset_id] = {};
    for (size_t j = 0; j < num_job_ids; ++j) {
      meta.dataset_job_ids[dataset_id].insert(read<i32>(file, pos));
    }
  }
  size_t num_jobs = read<size_t>(file, pos);
  for (size_t i = 0; i < num_jobs; ++i) {
    i32 job_id = read<i32>(file, pos);
    meta.job_names[job_id] = read<std::string>(file, pos);
  }

  return meta;
}

void serialize_dataset_descriptor(WriteFile* file,
                                  const DatasetDescriptor& descriptor) {
  write(file, descriptor.total_frames);

  write(file, descriptor.min_frames);
  write(file, descriptor.average_frames);
  write(file, descriptor.max_frames);

  write(file, descriptor.min_width);
  write(file, descriptor.average_width);
  write(file, descriptor.max_width);

  write(file, descriptor.min_height);
  write(file, descriptor.average_height);
  write(file, descriptor.max_height);

  // Number of videos
  size_t num_videos = descriptor.original_video_paths.size();
  write(file, num_videos);

  for (size_t i = 0; i < num_videos; ++i) {
    const std::string& path = descriptor.original_video_paths[i];
    const std::string& item_name = descriptor.item_names[i];

    write(file, path);
    write(file, item_name);
  }
}

DatasetDescriptor deserialize_dataset_descriptor(RandomReadFile* file,
                                                 uint64_t& pos) {
  DatasetDescriptor descriptor;

  descriptor.total_frames = read<int64_t>(file, pos);

  descriptor.min_frames = read<int32_t>(file, pos);
  descriptor.average_frames = read<int32_t>(file, pos);
  descriptor.max_frames = read<int32_t>(file, pos);

  descriptor.min_width = read<int32_t>(file, pos);
  descriptor.average_width = read<int32_t>(file, pos);
  descriptor.max_width = read<int32_t>(file, pos);

  descriptor.min_height = read<int32_t>(file, pos);
  descriptor.average_height = read<int32_t>(file, pos);
  descriptor.max_height = read<int32_t>(file, pos);

  // Size of metadata
  size_t num_videos = read<size_t>(file, pos);
  for (size_t i = 0; i < num_videos; ++i) {
    descriptor.original_video_paths.push_back(read<std::string>(file, pos));
    descriptor.item_names.push_back(read<std::string>(file, pos));
  }
  return descriptor;
}

void serialize_dataset_item_metadata(WriteFile* file,
                                     const DatasetItemMetadata& metadata) {
  write(file, metadata.frames);
  write(file, metadata.width);
  write(file, metadata.height);
  write(file, metadata.codec_type);
  write(file, metadata.chroma_format);

  StoreResult result;
  // Size of metadata
  size_t metadata_packets_size = metadata.metadata_packets.size();
  EXP_BACKOFF(file->append(sizeof(size_t),
                           reinterpret_cast<const u8*>(&metadata_packets_size)),
              result);
  exit_on_error(result);

  // Metadata packets
  EXP_BACKOFF(file->append(metadata_packets_size,
                           reinterpret_cast<const u8*>(
                               metadata.metadata_packets.data())),
              result);
  exit_on_error(result);

  // Keyframe info
  assert(metadata.keyframe_positions.size() ==
         metadata.keyframe_timestamps.size());
  assert(metadata.keyframe_positions.size() ==
         metadata.keyframe_byte_offsets.size());

  size_t num_keyframes = metadata.keyframe_positions.size();

  EXP_BACKOFF(
      file->append(sizeof(size_t), reinterpret_cast<u8*>(&num_keyframes)),
      result);
  exit_on_error(result);

  EXP_BACKOFF(file->append(sizeof(int64_t) * num_keyframes,
                           reinterpret_cast<const u8*>(
                               metadata.keyframe_positions.data())),
              result);
  exit_on_error(result);

  EXP_BACKOFF(file->append(sizeof(int64_t) * num_keyframes,
                           reinterpret_cast<const u8*>(
                               metadata.keyframe_timestamps.data())),
              result);
  exit_on_error(result);

  EXP_BACKOFF(file->append(sizeof(int64_t) * num_keyframes,
                           reinterpret_cast<const u8*>(
                               metadata.keyframe_byte_offsets.data())),
              result);
  exit_on_error(result);
}

DatasetItemMetadata deserialize_dataset_item_metadata(RandomReadFile* file,
                                                      uint64_t& pos) {
  StoreResult result;
  size_t size_read;

  DatasetItemMetadata meta;
  // Frames
  meta.frames = read<int32_t>(file, pos);
  meta.width = read<int32_t>(file, pos);
  meta.height = read<int32_t>(file, pos);
  meta.codec_type = read<VideoCodecType>(file, pos);
  meta.chroma_format = read<VideoChromaFormat>(file, pos);

  // Size of metadata
  size_t metadata_size;
  EXP_BACKOFF(file->read(pos, sizeof(size_t),
                         reinterpret_cast<u8*>(&metadata_size), size_read),
              result);
  exit_on_error(result);
  assert(size_read == sizeof(size_t));
  pos += size_read;

  // Metadata packets
  meta.metadata_packets.resize(metadata_size);
  EXP_BACKOFF(file->read(pos, metadata_size,
                         reinterpret_cast<u8*>(meta.metadata_packets.data()),
                         size_read),
              result);
  exit_on_error(result);
  assert(size_read == metadata_size);
  pos += size_read;

  size_t num_keyframes;
  // HACK(apoms): Reading just a single size_t is inefficient because
  //              the file interface does not buffer or preemptively fetch
  //              a larger block of data to amortize network overheads. We
  //              should instead read the entire file into a buffer because we
  //              know it is fairly small and then deserialize from there.
  EXP_BACKOFF(file->read(pos, sizeof(size_t),
                         reinterpret_cast<u8*>(&num_keyframes), size_read),
              result);
  exit_on_error(result);
  assert(size_read == sizeof(size_t));
  pos += size_read;

  meta.keyframe_positions.resize(num_keyframes);
  meta.keyframe_timestamps.resize(num_keyframes);
  meta.keyframe_byte_offsets.resize(num_keyframes);

  EXP_BACKOFF(file->read(pos, sizeof(int64_t) * num_keyframes,
                         reinterpret_cast<u8*>(meta.keyframe_positions.data()),
                         size_read),
              result);
  exit_on_error(result);
  assert(size_read == sizeof(int64_t) * num_keyframes);
  pos += size_read;

  EXP_BACKOFF(file->read(pos, sizeof(int64_t) * num_keyframes,
                         reinterpret_cast<u8*>(meta.keyframe_timestamps.data()),
                         size_read),
              result);
  assert(result == StoreResult::Success || result == StoreResult::EndOfFile);
  assert(size_read == sizeof(int64_t) * num_keyframes);
  pos += size_read;

  EXP_BACKOFF(
      file->read(pos, sizeof(int64_t) * num_keyframes,
                 reinterpret_cast<u8*>(meta.keyframe_byte_offsets.data()),
                 size_read),
      result);
  assert(result == StoreResult::Success || result == StoreResult::EndOfFile);
  assert(size_read == sizeof(int64_t) * num_keyframes);
  pos += size_read;

  return meta;
}

void serialize_dataset_item_web_timestamps(
    WriteFile* file, const DatasetItemWebTimestamps& metadata) {
  StoreResult result;
  EXP_BACKOFF(file->append(sizeof(int), reinterpret_cast<const u8*>(
                                            &metadata.time_base_numerator)),
              result);
  exit_on_error(result);

  // Width
  EXP_BACKOFF(file->append(sizeof(int), reinterpret_cast<const u8*>(
                                            &metadata.time_base_denominator)),
              result);
  exit_on_error(result);

  size_t num_frames = metadata.pts_timestamps.size();
  EXP_BACKOFF(file->append(sizeof(size_t), reinterpret_cast<u8*>(&num_frames)),
              result);
  exit_on_error(result);

  EXP_BACKOFF(
      file->append(sizeof(int64_t) * num_frames,
                   reinterpret_cast<const u8*>(metadata.pts_timestamps.data())),
      result);
  exit_on_error(result);

  EXP_BACKOFF(
      file->append(sizeof(int64_t) * num_frames,
                   reinterpret_cast<const u8*>(metadata.dts_timestamps.data())),
      result);
  exit_on_error(result);
}

DatasetItemWebTimestamps deserialize_dataset_item_web_timestamps(
    RandomReadFile* file, uint64_t& pos) {
  StoreResult result;
  size_t size_read;

  DatasetItemWebTimestamps meta;

  // timebase numerator
  EXP_BACKOFF(
      file->read(pos, sizeof(int),
                 reinterpret_cast<u8*>(&meta.time_base_numerator), size_read),
      result);
  exit_on_error(result);
  assert(size_read == sizeof(int));
  pos += size_read;

  // timebase denominator
  EXP_BACKOFF(
      file->read(pos, sizeof(int),
                 reinterpret_cast<u8*>(&meta.time_base_denominator), size_read),
      result);
  exit_on_error(result);
  assert(size_read == sizeof(int));
  pos += size_read;

  size_t num_frames;
  // Frames
  EXP_BACKOFF(file->read(pos, sizeof(size_t),
                         reinterpret_cast<u8*>(&num_frames), size_read),
              result);
  exit_on_error(result);
  assert(size_read == sizeof(size_t));
  pos += size_read;

  meta.pts_timestamps.resize(num_frames);
  meta.dts_timestamps.resize(num_frames);

  EXP_BACKOFF(
      file->read(pos, sizeof(int64_t) * num_frames,
                 reinterpret_cast<u8*>(meta.pts_timestamps.data()), size_read),
      result);
  exit_on_error(result);
  assert(size_read == sizeof(int64_t) * num_frames);
  pos += size_read;

  EXP_BACKOFF(
      file->read(pos, sizeof(int64_t) * num_frames,
                 reinterpret_cast<u8*>(meta.dts_timestamps.data()), size_read),
      result);
  assert(result == StoreResult::Success || result == StoreResult::EndOfFile);
  assert(size_read == sizeof(int64_t) * num_frames);
  pos += size_read;

  return meta;
}

void serialize_job_descriptor(WriteFile* file,
                              const JobDescriptor& descriptor) {
  StoreResult result;

  Json::Value root, videos;
  root["dataset_name"] = descriptor.dataset_name;

  for (auto it : descriptor.intervals) {
    const std::string& video_path = it.first;
    const std::vector<std::tuple<int, int>>& intervals = it.second;

    Json::Value video, json_intervals;
    video["path"] = video_path;

    for (const std::tuple<int, int>& interval : intervals) {
      Json::Value json_interval;
      json_interval.append(std::get<0>(interval));
      json_interval.append(std::get<1>(interval));
      json_intervals.append(json_interval);
    }

    video["intervals"] = json_intervals;
    videos.append(video);
  }

  root["videos"] = videos;

  Json::StreamWriterBuilder wbuilder;
  std::string doc = Json::writeString(wbuilder, root);

  EXP_BACKOFF(
      file->append(doc.size(), reinterpret_cast<const u8*>(doc.c_str())),
      result);
  exit_on_error(result);
}

JobDescriptor deserialize_job_descriptor(RandomReadFile* file,
                                         uint64_t& file_pos) {
  JobDescriptor descriptor;

  // Load the entire input
  std::vector<u8> bytes = read_entire_file(file, file_pos);

  std::istringstream ss(std::string(bytes.begin(), bytes.end()));
  Json::Value root;

  ss >> root;

  descriptor.dataset_name = root["dataset_name"].asString();

  const Json::Value videos = root["videos"];
  for (int i = 0; i < videos.size(); ++i) {
    std::string path = videos[i]["path"].asString();
    Json::Value json_intervals = videos[i]["intervals"];

    std::vector<std::tuple<int, int>> intervals;
    for (int j = 0; j < json_intervals.size(); ++j) {
      intervals.emplace_back(json_intervals[j][0].asInt64(),
                             json_intervals[j][1].asInt64());
    }

    descriptor.intervals[path] = intervals;
  }

  return descriptor;
}
}
