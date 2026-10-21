#pragma once

// Thin RAII layer over the HDF5 C API. HDF5 is built without its thread-safe
// option, so every call must hold h5_lock(); compression runs outside it.

#include <hdf5.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "raybotic/scan/types.hpp"

namespace raybotic::scan::h5 {

/// Process-wide lock around the HDF5 library. Recursive, so helpers can nest.
std::recursive_mutex& mutex();
using Lock = std::scoped_lock<std::recursive_mutex>;

/// Initialises the library once (silences HDF5's own error printing).
void init();

/// Throws Error with `what` and the HDF5 error stack if `status` < 0.
void check(herr_t status, std::string_view what);
hid_t check_id(hid_t id, std::string_view what);

/// Owns an hid_t and closes it with the matching H5*close.
class Id {
  public:
    Id() = default;
    explicit Id(hid_t id) : id_(id) {}
    ~Id() { reset(); }
    Id(Id&& o) noexcept : id_(o.id_) { o.id_ = H5I_INVALID_HID; }
    Id& operator=(Id&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = o.id_;
            o.id_ = H5I_INVALID_HID;
        }
        return *this;
    }
    Id(const Id&) = delete;
    Id& operator=(const Id&) = delete;

    [[nodiscard]] hid_t get() const { return id_; }
    [[nodiscard]] bool valid() const { return id_ >= 0; }
    explicit operator bool() const { return valid(); }
    void reset();

  private:
    hid_t id_{H5I_INVALID_HID};
};

/// File type (little-endian standard types, h5py-style bool enum).
Id file_type(DType dtype);
/// In-memory type matching the C++ element type.
Id memory_type(DType dtype);
/// The DType of a dataset or attribute type; throws for unsupported types.
DType dtype_of(hid_t type);

Id create_file(const std::string& path, bool overwrite, bool swmr_capable);
Id open_file(const std::string& path, unsigned flags, bool swmr_read = false);

bool exists(hid_t loc, const std::string& path);
/// Creates all missing groups along `path`.
Id ensure_group(hid_t loc, const std::string& path);
Id open_group(hid_t loc, const std::string& path);
Id open_dataset(hid_t loc, const std::string& path);
std::vector<std::string> children(hid_t group);
bool is_group(hid_t loc, const std::string& name);

/// Writes (replaces) an attribute. With `as`, the value is converted to that
/// dtype (array or scalar as given); otherwise the type follows the value.
void write_attribute(hid_t obj, const std::string& name, const Value& value,
                     std::optional<DType> as = std::nullopt, bool as_array = false);
void write_string_attribute(hid_t obj, const std::string& name, std::string_view value);
std::optional<Value> read_attribute(hid_t obj, const std::string& name);
std::optional<std::string> read_string_attribute(hid_t obj, const std::string& name);
Attributes read_attributes(hid_t obj);

/// Current extent of a dataset.
Shape extent(hid_t dataset);

}  // namespace raybotic::scan::h5
