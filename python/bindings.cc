#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>

#include "strata/vector_store.h"
#include "vector/hnsw_index.h"

namespace py = pybind11;

namespace strata {
namespace {

using Matrix = py::array_t<float, py::array::c_style | py::array::forcecast>;
using Pairs = std::vector<std::pair<std::string, std::string>>;

void Check(const Status& status) {
  if (status.ok()) {
    return;
  }
  const auto& message = status.message();
  if (status.code() == StatusCode::kInvalidArgument) {
    throw std::invalid_argument(message.empty() ? "invalid Strata argument" : message);
  }
  throw std::runtime_error(message.empty() ? "Strata storage operation failed" : message);
}

py::object Text(std::string_view text, bool raw) {
  if (raw) {
    return py::bytes(text.data(), text.size());
  }
  return py::str(text.data(), text.size());
}

class PythonDB {
 public:
  PythonDB(const std::filesystem::path& path, VectorOptions options) {
    py::gil_scoped_release release;
    Check(DB::Open({}, path.string(), &db_));
    Check(VectorStore::Open({}, options, path.string(), &vectors_));
  }

  void Put(const std::string& key, const std::string& value) {
    py::gil_scoped_release release;
    Check(db_->Put(key, value));
  }

  py::object Get(const std::string& key, bool raw) {
    std::string value;
    Status status;
    {
      py::gil_scoped_release release;
      status = db_->Get(key, &value);
    }
    if (status.code() == StatusCode::kNotFound) {
      return py::none();
    }
    Check(status);
    return Text(value, raw);
  }

  void Delete(const std::string& key) {
    py::gil_scoped_release release;
    Check(db_->Delete(key));
  }

  py::list Scan(const std::string& start, const std::string& end, bool raw) {
    Pairs rows;
    {
      py::gil_scoped_release release;
      Check(db_->Scan(start, end, &rows));
    }
    py::list result;
    for (const auto& [key, value] : rows) {
      result.append(py::make_tuple(Text(key, raw), Text(value, raw)));
    }
    return result;
  }

  void VectorPut(const std::string& id, const std::vector<float>& values) {
    py::gil_scoped_release release;
    Check(vectors_->Put(id, values));
  }

  py::list VectorSearch(const std::vector<float>& query, std::size_t k, std::size_t ef, bool raw) {
    std::vector<VectorHit> hits;
    {
      py::gil_scoped_release release;
      Check(vectors_->Search(query, k, &hits, ef));
    }
    py::list result;
    for (const auto& [id, distance] : hits) {
      result.append(py::make_tuple(Text(id, raw), distance));
    }
    return result;
  }

 private:
  std::unique_ptr<DB> db_;
  std::unique_ptr<VectorStore> vectors_;
};

template <class Index>
class EvaluationIndex {
 public:
  template <class Config>
  explicit EvaluationIndex(Config config) : index_(config) {}

  void Add(const Matrix& data) {
    if (data.ndim() != 2 || data.shape(1) < 1 || data.shape(1) > 4096) {
      throw std::invalid_argument("vectors must be a two-dimensional float array");
    }
    const auto rows = static_cast<std::size_t>(data.shape(0));
    const auto dim = static_cast<std::size_t>(data.shape(1));
    const auto* input = data.data();
    py::gil_scoped_release release;
    std::unique_lock lock(mutex_);
    if (dimension_ != 0 && dimension_ != dim) {
      throw std::invalid_argument("dimension does not match the index");
    }
    for (std::size_t row = 0; row < rows; ++row) {
      if (!vector::ValidVector({input + row * dim, dim})) {
        throw std::invalid_argument("invalid vector component");
      }
    }
    for (std::size_t row = 0; row < rows; ++row) {
      index_.Insert({input + row * dim, dim});
      ++size_;
      dimension_ = dim;
    }
  }

  py::array_t<std::uint32_t> Search(const Matrix& data, std::size_t k, std::size_t ef) const {
    if (data.ndim() != 2 || data.shape(1) < 1 || data.shape(1) > 4096 || k == 0 || k > 65536 ||
        ef > 65536) {
      throw std::invalid_argument("invalid query matrix or search size");
    }
    const auto rows = static_cast<std::size_t>(data.shape(0));
    const auto dim = static_cast<std::size_t>(data.shape(1));
    const auto* input = data.data();
    py::array_t<std::uint32_t> output(
        py::array::ShapeContainer{data.shape(0), static_cast<py::ssize_t>(k)});
    auto* ids = output.mutable_data();
    {
      py::gil_scoped_release release;
      std::shared_lock lock(mutex_);
      if (dim != dimension_ || k > size_) {
        throw std::invalid_argument("query dimension or k does not match the index");
      }
      for (std::size_t row = 0; row < rows; ++row) {
        const std::span<const float> query(input + row * dim, dim);
        if (!vector::ValidVector(query)) {
          throw std::invalid_argument("invalid query vector component");
        }
        const auto hits = [&] {
          if constexpr (std::is_same_v<Index, vector::HnswIndex>) {
            return index_.Search(query, k, ef);
          } else {
            return index_.Search(query, k);
          }
        }();
        for (std::size_t i = 0; i < k; ++i) {
          ids[row * k + i] = hits[i].second;
        }
      }
    }
    return output;
  }

 private:
  Index index_;
  std::size_t dimension_ = 0;
  std::size_t size_ = 0;
  mutable std::shared_mutex mutex_;
};

}  // namespace

PYBIND11_MODULE(strata, module) {
  module.doc() = "Strata persistent key-value and HNSW vector search";
  py::enum_<Metric>(module, "Metric").value("L2", Metric::kL2).value("COSINE", Metric::kCosine);
  py::class_<PythonDB>(module, "DB")
      .def(py::init([](const std::filesystem::path& path, Metric metric, std::size_t m,
                       std::size_t ef_construction, std::size_t ef_search) {
             return std::make_unique<PythonDB>(
                 path, VectorOptions{m, ef_construction, ef_search, metric});
           }),
           py::arg("path"), py::kw_only(), py::arg("metric") = Metric::kCosine, py::arg("m") = 16,
           py::arg("ef_construction") = 100, py::arg("ef_search") = 64)
      .def("put", &PythonDB::Put, py::arg("key"), py::arg("value"))
      .def("get", &PythonDB::Get, py::arg("key"), py::kw_only(), py::arg("raw") = false)
      .def("delete", &PythonDB::Delete, py::arg("key"))
      .def("scan", &PythonDB::Scan, py::arg("start"), py::arg("end"), py::kw_only(),
           py::arg("raw") = false)
      .def("vput", &PythonDB::VectorPut, py::arg("id"), py::arg("values"))
      .def("vsearch", &PythonDB::VectorSearch, py::arg("query"), py::arg("k"), py::kw_only(),
           py::arg("ef_search") = 0, py::arg("raw") = false);

  using Flat = EvaluationIndex<vector::FlatIndex>;
  using Graph = EvaluationIndex<vector::HnswIndex>;
  py::class_<Flat>(module, "_FlatIndex")
      .def(py::init<Metric>())
      .def("add", &Flat::Add)
      .def("search", &Flat::Search, py::arg("queries"), py::arg("k"), py::arg("ef_search") = 0);
  py::class_<Graph>(module, "_HnswIndex")
      .def(py::init([](Metric metric, std::size_t m, std::size_t ef_construction) {
        VectorOptions options{m, ef_construction, 64, metric};
        if (!vector::ValidOptions(options)) {
          throw std::invalid_argument("invalid HNSW options");
        }
        return std::make_unique<Graph>(options);
      }))
      .def("add", &Graph::Add)
      .def("search", &Graph::Search, py::arg("queries"), py::arg("k"), py::arg("ef_search") = 0);
}

}  // namespace strata
