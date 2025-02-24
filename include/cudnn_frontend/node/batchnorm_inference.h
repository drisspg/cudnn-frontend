#pragma once

#include "../../cudnn_frontend_Heuristics.h"
#include "../../cudnn_frontend_Logging.h"

#include "../graph_helpers.h"
#include "../node_interface.h"

namespace cudnn_frontend {

namespace graph {
class BatchnormInferenceNode : public INode {
   public:
    Batchnorm_inference_attributes attributes;

    BatchnormInferenceNode(Batchnorm_inference_attributes&& attributes_, detail::Context const& context)
        : INode(context), attributes(std::move(attributes_)) {}

    Type
    getType() override final {
        return Type::BATCHNORM_INFERENCE;
    }

    error_t
    expand_and_infer_properties() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferencing properties for batchnorm inference node " << attributes.name
                    << "..." << std::endl;

        attributes.fill_from_context(context);

        auto X                  = attributes.inputs[Batchnorm_inference_attributes::input_names::X];
        auto const x_tensor_dim = X->get_dim();

        auto Y            = attributes.outputs[Batchnorm_inference_attributes::output_names::Y];
        auto y_tensor_dim = Y->get_dim();
        // Only infer dims and strides if user did not set them
        if (y_tensor_dim.empty()) {
            y_tensor_dim.resize(x_tensor_dim.size());
            Y->set_dim(x_tensor_dim);
        }
        if (Y->get_stride().empty()) {
            auto const& Y_dim = Y->get_dim();
            // Default to NHWC
            auto const& stride_order = detail::generate_NHWC_stride_order(Y_dim.size());
            Y->set_stride(detail::generate_stride(Y_dim, stride_order));
        }

        return {error_code_t::OK, ""};
    }

    error_t
    pre_validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating BatchnormInferenceNode " << attributes.name << "..." << std::endl;
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Batchnorm_inference_attributes::input_names::X);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Batchnorm_inference_attributes::input_names::SCALE);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Batchnorm_inference_attributes::input_names::BIAS);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Batchnorm_inference_attributes::input_names::MEAN);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Batchnorm_inference_attributes::input_names::INV_VARIANCE);

        CUDNN_FE_VALIDATE_OUTPUT_TENSOR(Batchnorm_inference_attributes::output_names::Y);

        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_inputs());

        return {error_code_t::OK, ""};
    }

    error_t
    post_validate_node() const override final {
        // Validate outputs
        // All properties of output tensors should have been set now.
        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_outputs());

        return {error_code_t::OK, ""};
    }

    error_t
    create_cudnn_tensors(int64_t& uid, std::unordered_map<int64_t, std::shared_ptr<cudnn_frontend::Tensor>>& tensors)
        const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building BatchnormInferenceNode tensors " << attributes.name << "..." << std::endl;

        for (auto const& [name, tensor] : attributes.inputs) {
            (void)name;
            if (tensor) {
                CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(tensor, uid, tensors));
            }
        }
        for (auto const& [name, tensor] : attributes.outputs) {
            (void)name;
            if (tensor) {
                CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(tensor, uid, tensors));
            }
        }
        return {error_code_t::OK, ""};
    }

    error_t
    create_cudnn_operations(
        std::unordered_set<uid_t>& uids_involved_in_operations,
        std::vector<std::shared_ptr<cudnn_frontend::Operation>>& operations,
        std::unordered_map<int64_t, std::shared_ptr<cudnn_frontend::Tensor>>& tensors) const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building BatchnormInferenceNode operations " << attributes.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            auto&& batchnorm_operation_builder =
                cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_NORM_FORWARD_DESCRIPTOR);
            batchnorm_operation_builder.setNormalizationMode(NormMode_t::BATCH_NORM)
                .setNormFwdPhase(NormFwdPhase_t::INFERENCE);

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(X, Batchnorm_inference_attributes::input_names::X);
            batchnorm_operation_builder.setxDesc(*(tensors.at(X->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(MEAN, Batchnorm_inference_attributes::input_names::MEAN);
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(INV_VARIANCE,
                                                      Batchnorm_inference_attributes::input_names::INV_VARIANCE);
            batchnorm_operation_builder.setSavedMeanAndInvVar(*(tensors.at(MEAN->second->get_uid())),
                                                              *(tensors.at(INV_VARIANCE->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(SCALE, Batchnorm_inference_attributes::input_names::SCALE);
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(BIAS, Batchnorm_inference_attributes::input_names::BIAS);
            batchnorm_operation_builder.setScaleAndBias(*(tensors.at(SCALE->second->get_uid())),
                                                        *(tensors.at(BIAS->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_OUTPUT_TENSOR(Y, Batchnorm_inference_attributes::output_names::Y);
            batchnorm_operation_builder.setyDesc(*(tensors.at(Y->second->get_uid())));

            auto operation = batchnorm_operation_builder.build();

            operations.push_back(std::make_shared<Operation_v8>(std::move(operation)));

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        } catch (cudnn_frontend::cudnnException& e) {
            throw cudnnException(e.what(), e.getCudnnStatus());
        }
#endif

        auto const& non_virtual_uids = attributes.get_non_virtual_uids();
        uids_involved_in_operations.insert(non_virtual_uids.begin(), non_virtual_uids.end());
        return {error_code_t::OK, ""};
    }

    virtual void
    serialize(json& j) const override final {
        j = attributes;
    }
};

}  // namespace graph

}  // namespace cudnn_frontend