#pragma once

#include "../../cudnn_frontend_ConvDesc.h"
#include "../../cudnn_frontend_Heuristics.h"
#include "../../cudnn_frontend_Logging.h"

#include "../graph_helpers.h"
#include "../node_interface.h"

namespace cudnn_frontend::graph {

class DgradNode : public INode {
    Conv_dgrad_attributes attributes;

   public:
    DgradNode(Conv_dgrad_attributes&& attributes_, detail::Context const& context)
        : INode(context), attributes(std::move(attributes_)) {}

    Type
    getType() override final {
        return Type::DGRAD;
    }

    error_t
    pre_validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating Node Type::DGRAD " << attributes.name << "..." << std::endl;

        CUDNN_FE_VALIDATE_INPUT_TENSOR(Conv_dgrad_attributes::input_names::DY);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(Conv_dgrad_attributes::input_names::W);

        CUDNN_FE_VALIDATE_OUTPUT_TENSOR(Conv_dgrad_attributes::output_names::DX);

        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_inputs());
        return {error_code_t::OK, ""};
    }

    error_t
    expand_and_infer_properties() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferrencing properties for dgrad node " << attributes.name << "..."
                    << std::endl;

        attributes.fill_from_context(context);

        // TODO: Only inferrencing from (X, DY) -> DW works today.
        auto DX = attributes.outputs.find(Conv_dgrad_attributes::output_names::DX)->second;
        auto W  = attributes.inputs.find(Conv_dgrad_attributes::input_names::W)->second;
        auto DY = attributes.inputs.find(Conv_dgrad_attributes::input_names::DY)->second;

        auto const w_tensor_dim  = W->get_dim();
        auto const dy_tensor_dim = DY->get_dim();
        auto dx_tensor_dim       = DX->get_dim();

        // No dim inferencing as inverse mapping from DY, W to DX is not unique.
        // Only infer strides if user did not set them
        if (DX->get_stride().empty()) {
            auto const& DX_dim = DX->get_dim();
            // Default to NHWC
            auto const& stride_order = detail::generate_NHWC_stride_order(DX_dim.size());
            DX->set_stride(detail::generate_stride(DX_dim, stride_order));
        }

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
                    << "Building DgradNode tensors " << attributes.name << "..." << std::endl;

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
                    << "Building DgradNode operations " << attributes.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            // dgrad descriptor
            int64_t const spatial_dim_count = attributes.get_padding().size();
            auto dgrad_descriptor           = cudnn_frontend::ConvDescBuilder()
                                        .setComputeType(attributes.compute_data_type)
                                        .setMathMode(CUDNN_CROSS_CORRELATION)
                                        .setSpatialDimCount(spatial_dim_count)
                                        .setSpatialStride(spatial_dim_count, attributes.get_stride().data())
                                        .setPrePadding(spatial_dim_count, attributes.get_padding().data())
                                        .setPostPadding(spatial_dim_count, attributes.get_padding().data())
                                        .setDilation(spatial_dim_count, attributes.get_dilation().data())
                                        .build();

            // Create the dgrad operation.
            auto&& dgrad_operation_builder =
                cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_CONVOLUTION_BACKWARD_DATA_DESCRIPTOR);

            CUDNN_FE_VALIDATE_AND_ASSIGN_OUTPUT_TENSOR(DX, Conv_dgrad_attributes::output_names::DX);
            dgrad_operation_builder.setdxDesc(*(tensors.at(DX->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(W, Conv_dgrad_attributes::input_names::W);
            dgrad_operation_builder.setwDesc(*(tensors.at(W->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(DY, Conv_dgrad_attributes::input_names::DY);
            dgrad_operation_builder.setdyDesc(*(tensors.at(DY->second->get_uid())));

            dgrad_operation_builder.setcDesc(dgrad_descriptor).setAlpha(1.f).setBeta(0.f);

            auto operation = dgrad_operation_builder.build();

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

}  // namespace cudnn_frontend::graph