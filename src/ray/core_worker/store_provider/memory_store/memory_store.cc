#include <condition_variable>
#include "ray/common/ray_config.h"
#include "ray/core_worker/context.h"
#include "ray/core_worker/core_worker.h"
#include "ray/core_worker/store_provider/memory_store_provider.h"

namespace ray {

/// A class that represents a `Get` request.
class GetRequest {
 public:
  GetRequest(absl::flat_hash_set<ObjectID> object_ids, size_t num_objects,
             bool remove_after_get);

  const absl::flat_hash_set<ObjectID> &ObjectIds() const;

  /// Wait until all requested objects are available, or timeout happens.
  ///
  /// \param timeout_ms The maximum time in milliseconds to wait for.
  /// \return Whether all requested objects are available.
  bool Wait(int64_t timeout_ms);
  /// Set the object content for the specific object id.
  void Set(const ObjectID &object_id, std::shared_ptr<RayObject> buffer);
  /// Get the object content for the specific object id.
  std::shared_ptr<RayObject> Get(const ObjectID &object_id) const;
  /// Whether this is a `get` request.
  bool ShouldRemoveObjects() const;

 private:
  /// Wait until all requested objects are available.
  void Wait();

  /// The object IDs involved in this request.
  const absl::flat_hash_set<ObjectID> object_ids_;
  /// The object information for the objects in this request.
  absl::flat_hash_map<ObjectID, std::shared_ptr<RayObject>> objects_;
  /// Number of objects required.
  const size_t num_objects_;

  // Whether the requested objects should be removed from store
  // after `get` returns.
  const bool remove_after_get_;
  // Whether all the requested objects are available.
  bool is_ready_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

GetRequest::GetRequest(absl::flat_hash_set<ObjectID> object_ids, size_t num_objects,
                       bool remove_after_get)
    : object_ids_(std::move(object_ids)),
      num_objects_(num_objects),
      remove_after_get_(remove_after_get),
      is_ready_(false) {
  RAY_CHECK(num_objects_ <= object_ids_.size());
}

const absl::flat_hash_set<ObjectID> &GetRequest::ObjectIds() const { return object_ids_; }

bool GetRequest::ShouldRemoveObjects() const { return remove_after_get_; }

bool GetRequest::Wait(int64_t timeout_ms) {
  RAY_CHECK(timeout_ms >= 0 || timeout_ms == -1);
  if (timeout_ms == -1) {
    // Wait forever until all objects are ready.
    Wait();
    return true;
  }

  // Wait until all objects are ready, or the timeout expires.
  std::unique_lock<std::mutex> lock(mutex_);
  while (!is_ready_) {
    auto status = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    if (status == std::cv_status::timeout) {
      return false;
    }
  }
  return true;
}

void GetRequest::Wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!is_ready_) {
    cv_.wait(lock);
  }
}

void GetRequest::Set(const ObjectID &object_id, std::shared_ptr<RayObject> object) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (is_ready_) {
    return;  // We have already hit the number of objects to return limit.
  }
  objects_.emplace(object_id, object);
  if (objects_.size() == num_objects_) {
    is_ready_ = true;
    cv_.notify_all();
  }
}

std::shared_ptr<RayObject> GetRequest::Get(const ObjectID &object_id) const {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = objects_.find(object_id);
  if (iter != objects_.end()) {
    return iter->second;
  }

  return nullptr;
}

CoreWorkerMemoryStore::CoreWorkerMemoryStore(
    std::function<void(const RayObject &, const ObjectID &)> store_in_plasma)
    : store_in_plasma_(store_in_plasma) {}

void CoreWorkerMemoryStore::GetAsync(
    const ObjectID &object_id, std::function<void(std::shared_ptr<RayObject>)> callback) {
  std::shared_ptr<RayObject> ptr;
  {
    absl::MutexLock lock(&mu_);
    auto iter = objects_.find(object_id);
    if (iter != objects_.end()) {
      ptr = iter->second;
    } else {
      object_async_get_requests_[object_id].push_back(callback);
    }
  }
  // It's important for performance to run the callback outside the lock.
  if (ptr != nullptr) {
    callback(ptr);
  }
}

std::shared_ptr<RayObject> CoreWorkerMemoryStore::GetOrPromoteToPlasma(
    const ObjectID &object_id) {
  absl::MutexLock lock(&mu_);
  auto iter = objects_.find(object_id);
  if (iter != objects_.end()) {
    auto obj = iter->second;
    if (obj->IsInPlasmaError()) {
      return nullptr;
    }
    return obj;
  }
  RAY_CHECK(store_in_plasma_ != nullptr)
      << "Cannot promote object without plasma provider callback.";
  promoted_to_plasma_.insert(object_id);
  return nullptr;
}

Status CoreWorkerMemoryStore::Put(const ObjectID &object_id, const RayObject &object) {
  RAY_CHECK(object_id.IsDirectCallType());
  std::vector<std::function<void(std::shared_ptr<RayObject>)>> async_callbacks;
  auto object_entry =
      std::make_shared<RayObject>(object.GetData(), object.GetMetadata(), true);

  {
    absl::MutexLock lock(&mu_);
    auto iter = objects_.find(object_id);
    if (iter != objects_.end()) {
      return Status::ObjectExists("object already exists in the memory store");
    }

    auto async_callback_it = object_async_get_requests_.find(object_id);
    if (async_callback_it != object_async_get_requests_.end()) {
      auto &callbacks = async_callback_it->second;
      async_callbacks = std::move(callbacks);
      object_async_get_requests_.erase(async_callback_it);
    }

    auto promoted_it = promoted_to_plasma_.find(object_id);
    if (promoted_it != promoted_to_plasma_.end()) {
      RAY_CHECK(store_in_plasma_ != nullptr);
      store_in_plasma_(object, object_id.WithTransportType(TaskTransportType::RAYLET));
      promoted_to_plasma_.erase(promoted_it);
    }

    bool should_add_entry = true;
    auto object_request_iter = object_get_requests_.find(object_id);
    if (object_request_iter != object_get_requests_.end()) {
      auto &get_requests = object_request_iter->second;
      for (auto &get_request : get_requests) {
        get_request->Set(object_id, object_entry);
        if (get_request->ShouldRemoveObjects()) {
          should_add_entry = false;
        }
      }
    }

    if (should_add_entry) {
      // If there is no existing get request, then add the `RayObject` to map.
      objects_.emplace(object_id, object_entry);
    }
  }

  // It's important for performance to run the callbacks outside the lock.
  for (const auto &cb : async_callbacks) {
    cb(object_entry);
  }

  return Status::OK();
}

Status CoreWorkerMemoryStore::Get(const std::vector<ObjectID> &object_ids,
                                  int num_objects, int64_t timeout_ms,
                                  bool remove_after_get,
                                  std::vector<std::shared_ptr<RayObject>> *results) {
  (*results).resize(object_ids.size(), nullptr);

  std::shared_ptr<GetRequest> get_request;
  int count = 0;

  {
    absl::flat_hash_set<ObjectID> remaining_ids;
    absl::flat_hash_set<ObjectID> ids_to_remove;

    absl::MutexLock lock(&mu_);
    // Check for existing objects and see if this get request can be fullfilled.
    for (size_t i = 0; i < object_ids.size() && count < num_objects; i++) {
      const auto &object_id = object_ids[i];
      auto iter = objects_.find(object_id);
      if (iter != objects_.end()) {
        (*results)[i] = iter->second;
        if (remove_after_get) {
          // Note that we cannot remove the object_id from `objects_` now,
          // because `object_ids` might have duplicate ids.
          ids_to_remove.insert(object_id);
        }
        count += 1;
      } else {
        remaining_ids.insert(object_id);
      }
    }
    RAY_CHECK(count <= num_objects);

    for (const auto &object_id : ids_to_remove) {
      objects_.erase(object_id);
    }

    // Return if all the objects are obtained.
    if (remaining_ids.empty() || count >= num_objects) {
      return Status::OK();
    }

    size_t required_objects = num_objects - (object_ids.size() - remaining_ids.size());

    // Otherwise, create a GetRequest to track remaining objects.
    get_request = std::make_shared<GetRequest>(std::move(remaining_ids), required_objects,
                                               remove_after_get);
    for (const auto &object_id : get_request->ObjectIds()) {
      object_get_requests_[object_id].push_back(get_request);
    }
  }

  // Wait for remaining objects (or timeout).
  bool done = get_request->Wait(timeout_ms);

  {
    absl::MutexLock lock(&mu_);
    // Populate results.
    for (size_t i = 0; i < object_ids.size(); i++) {
      const auto &object_id = object_ids[i];
      if ((*results)[i] == nullptr) {
        (*results)[i] = get_request->Get(object_id);
      }
    }

    // Remove get request.
    for (const auto &object_id : get_request->ObjectIds()) {
      auto object_request_iter = object_get_requests_.find(object_id);
      if (object_request_iter != object_get_requests_.end()) {
        auto &get_requests = object_request_iter->second;
        // Erase get_request from the vector.
        auto it = std::find(get_requests.begin(), get_requests.end(), get_request);
        if (it != get_requests.end()) {
          get_requests.erase(it);
          // If the vector is empty, remove the object ID from the map.
          if (get_requests.empty()) {
            object_get_requests_.erase(object_request_iter);
          }
        }
      }
    }
  }

  if (done) {
    return Status::OK();
  } else {
    return Status::TimedOut("Get timed out: some object(s) not ready.");
  }
}

void CoreWorkerMemoryStore::Delete(const std::vector<ObjectID> &object_ids) {
  absl::MutexLock lock(&mu_);
  for (const auto &object_id : object_ids) {
    objects_.erase(object_id);
  }
}

bool CoreWorkerMemoryStore::Contains(const ObjectID &object_id) {
  absl::MutexLock lock(&mu_);
  auto it = objects_.find(object_id);
  // If obj is in plasma, we defer to the plasma store for the Contains() call.
  return it != objects_.end() && !it->second->IsInPlasmaError();
}

}  // namespace ray
