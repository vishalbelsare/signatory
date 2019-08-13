#include <torch/extension.h>
#include <Python.h>   // PyCapsule
#include <cstdint>    // int64_t
#include <stdexcept>  // std::invalid_argument
#include <tuple>      // std::tuple
#include <vector>     // std::vector

#include "misc.hpp"
#include "utilities.hpp"


namespace signatory {
    namespace misc {
        namespace detail {
            constexpr auto backwards_info_capsule_name = "signatory.BackwardsInfoCapsule";

            void BackwardsInfoCapsuleDestructor(PyObject* capsule) {
                delete static_cast<BackwardsInfo*>(PyCapsule_GetPointer(capsule, backwards_info_capsule_name));
            }
        }  // namespace signatory::misc::detail

        SigSpec::SigSpec(torch::Tensor path, size_type depth, bool stream, bool basepoint) :
            opts{torch::TensorOptions().dtype(path.dtype()).device(path.device())},
            input_stream_size{path.size(0)},
            input_channels{path.size(1)},
            batch_size{path.size(2)},
            output_stream_size{path.size(0) - (basepoint ? 0 : 1)},
            output_channels{signature_channels(path.size(1), depth)},
            n_output_dims{stream ? 3 : 2},
            depth{depth},
            reciprocals{torch::ones({depth - 1}, opts)},
            stream{stream},
            basepoint{basepoint}
        {
            if (depth > 1) {
                reciprocals /= torch::linspace(2, depth, depth - 1, opts);
            }  // and reciprocals will be empty - of size 0 - if depth == 1.
        };

        void slice_by_term(torch::Tensor in, std::vector <torch::Tensor> &out, int64_t dim, const SigSpec &sigspec) {
            int64_t current_memory_pos = 0;
            int64_t current_memory_length = sigspec.input_channels;
            out.clear();
            out.reserve(sigspec.depth);
            for (int64_t i = 0; i < sigspec.depth; ++i) {
                out.push_back(in.narrow(/*dim=*/dim,
                        /*start=*/current_memory_pos,
                        /*len=*/current_memory_length));
                current_memory_pos += current_memory_length;
                current_memory_length *= sigspec.input_channels;
            }
        }

        void slice_at_stream(std::vector <torch::Tensor> in, std::vector <torch::Tensor> &out, int64_t stream_index) {
            out.clear();
            out.reserve(in.size());
            for (auto elem : in) {
                out.push_back(elem.narrow(/*dim=*/0, /*start=*/stream_index, /*len=*/1).squeeze(0));
            }
        }

        torch::Tensor transpose(torch::Tensor tensor, const SigSpec &sigspec) {
            if (sigspec.stream) {
                // convert from (stream, channel, batch) to (batch, stream, channel)
                return tensor.transpose(1, 2).transpose(0, 1);
            } else {
                // convert from (channel, batch) to (batch, channel)
                return tensor.transpose(0, 1);
            }
        }

        torch::Tensor transpose_reverse(torch::Tensor tensor, const SigSpec &sigspec) {
            if (sigspec.stream) {
                // convert from (batch, stream, channel) to (stream, channel, batch)
                return tensor.transpose(0, 1).transpose(1, 2);
            } else {
                // convert from (batch, channel) to (channel, batch)
                return tensor.transpose(0, 1);
            }
        }

        bool is_even(size_type index) {
            return (((index) % 2) == 0);
        }

        BackwardsInfo::BackwardsInfo(SigSpec&& sigspec, std::vector<torch::Tensor>&& out_vector, torch::Tensor out,
                                     torch::Tensor path_increments) :
            sigspec{sigspec},
            out_vector{out_vector},
            out{out},
            path_increments{path_increments}
            {};

        void BackwardsInfo::set_logsignature_data(std::vector<torch::Tensor>&& signature_vector_,
                                                  std::vector<std::tuple<int64_t, int64_t, int64_t>>&& transforms_,
                                                  LogSignatureMode mode_,
                                                  int64_t logsignature_channels_) {
            signature_vector = signature_vector_;
            transforms = transforms_;
            mode = mode_;
            logsignature_channels = logsignature_channels_;
        }

        py::object make_backwards_info(std::vector<torch::Tensor>& out_vector, torch::Tensor out,
                                       torch::Tensor path_increments, SigSpec& sigspec) {
            return py::reinterpret_steal<py::object>(PyCapsule_New(new misc::BackwardsInfo{std::move(sigspec),
                                                                                           std::move(out_vector),
                                                                                           out,
                                                                                           path_increments},
                                                                   detail::backwards_info_capsule_name,
                                                                   detail::BackwardsInfoCapsuleDestructor));
        }

        BackwardsInfo* get_backwards_info(py::object backwards_info_capsule) {
            return static_cast<BackwardsInfo*>(
                    PyCapsule_GetPointer(backwards_info_capsule.ptr(), detail::backwards_info_capsule_name));
        }

        void checkargs(torch::Tensor path, size_type depth, bool basepoint, torch::Tensor basepoint_value) {
            if (path.ndimension() != 3) {
                throw std::invalid_argument("Argument 'path' must be a 3-dimensional tensor, with dimensions "
                                            "corresponding to (batch, stream, channel) respectively.");
            }
            if (path.size(0) == 0 || path.size(1) == 0 || path.size(2) == 0) {
                throw std::invalid_argument("Argument 'path' cannot have dimensions of size zero.");
            }
            if (!basepoint && path.size(1) == 1) {
                throw std::invalid_argument("Argument 'path' must have stream dimension of size at least 2. (Need at "
                                            "least this many points to define a path.)");
            }
            if (depth < 1) {
                throw std::invalid_argument("Argument 'depth' must be an integer greater than or equal to one.");
            }
            if (basepoint) {
                if (basepoint_value.ndimension() != 2) {
                    throw std::invalid_argument("Argument 'basepoint' must be a 2-dimensional tensor, corresponding to "
                                                "(batch, channel) respectively.");
                }
                // basepoint_value has dimensions (batch, channel)
                // path has dimensions (batch, stream, channel)
                if (basepoint_value.size(0) != path.size(0) || basepoint_value.size(1) != path.size(2)) {
                    throw std::invalid_argument("Arguments 'basepoint' and 'path' must have dimensions of the same "
                                                "size.");
                }
            }
        }

        void checkargs_backward(torch::Tensor grad_out, const SigSpec& sigspec, int64_t num_channels) {
            if (num_channels == -1) {
                num_channels = sigspec.output_channels;
            }

            if (sigspec.stream) {
                if (grad_out.ndimension() != 3) {
                    throw std::invalid_argument("Gradient must be a 3-dimensional tensor, with dimensions "
                                                "corresponding to (batch, stream, channel) respectively.");
                }
                if (grad_out.size(0) != sigspec.batch_size ||
                    grad_out.size(1) != sigspec.output_stream_size ||
                    grad_out.size(2) != num_channels) {
                    throw std::invalid_argument("Gradient has the wrong size.");
                }
            }
            else {
                if (grad_out.ndimension() != 2) {
                    throw std::invalid_argument("Gradient must be a 2-dimensional tensor, with dimensions"
                                                "corresponding to (batch, channel) respectively.");
                }
                if (grad_out.size(0) != sigspec.batch_size ||
                    grad_out.size(1) != num_channels) {
                    throw std::invalid_argument("Gradient has the wrong size.");
                }
            }
        }
    }  // namespace signatory::misc
}  // namespace signatory