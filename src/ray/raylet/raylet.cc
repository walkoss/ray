// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/raylet/raylet.h"

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ray/common/client_connection.h"
#include "ray/common/scheduling/resource_set.h"
#include "ray/common/status.h"
#include "ray/object_manager/object_manager.h"
#include "ray/object_manager/ownership_object_directory.h"
#include "ray/object_manager/plasma/client.h"
#include "ray/util/util.h"

namespace {

const std::vector<std::string> GenerateEnumNames(const char *const *enum_names_ptr,
                                                 int start_index,
                                                 int end_index) {
  std::vector<std::string> enum_names;
  for (int i = 0; i < start_index; ++i) {
    enum_names.push_back("EmptyMessageType");
  }
  size_t i = 0;
  while (true) {
    const char *name = enum_names_ptr[i];
    if (name == nullptr) {
      break;
    }
    enum_names.push_back(name);
    i++;
  }
  RAY_CHECK(static_cast<size_t>(end_index) == enum_names.size() - 1)
      << "Message Type mismatch!";
  return enum_names;
}

static const std::vector<std::string> node_manager_message_enum =
    GenerateEnumNames(ray::protocol::EnumNamesMessageType(),
                      static_cast<int>(ray::protocol::MessageType::MIN),
                      static_cast<int>(ray::protocol::MessageType::MAX));
}  // namespace

namespace ray {

namespace raylet {

Raylet::Raylet(instrumented_io_context &main_service,
               const NodeID &self_node_id,
               const std::string &socket_name,
               const std::string &node_ip_address,
               const std::string &node_name,
               const NodeManagerConfig &node_manager_config,
               const ObjectManagerConfig &object_manager_config,
               std::shared_ptr<gcs::GcsClient> gcs_client,
               int metrics_export_port,
               bool is_head_node,
               std::function<void(const rpc::NodeDeathInfo &)> shutdown_raylet_gracefully)
    : self_node_id_(self_node_id),
      gcs_client_(std::move(gcs_client)),
      socket_name_(socket_name),
      acceptor_(main_service, ParseUrlEndpoint(socket_name)),
      socket_(main_service),
      client_call_manager_(main_service, /*record_stats=*/true),
      worker_rpc_pool_([this](const rpc::Address &addr) {
        return std::make_shared<rpc::CoreWorkerClient>(
            addr,
            client_call_manager_,
            rpc::CoreWorkerClientPool::GetDefaultUnavailableTimeoutCallback(
                gcs_client_.get(),
                &worker_rpc_pool_,
                [this](const std::string &node_manager_address, int32_t port) {
                  return std::make_shared<raylet::RayletClient>(
                      rpc::NodeManagerWorkerClient::make(
                          node_manager_address, port, client_call_manager_));
                },
                addr));
      }) {
  auto core_worker_subscriber = std::make_unique<pubsub::Subscriber>(
      self_node_id_,
      /*channels=*/
      std::vector<rpc::ChannelType>{rpc::ChannelType::WORKER_OBJECT_EVICTION,
                                    rpc::ChannelType::WORKER_REF_REMOVED_CHANNEL,
                                    rpc::ChannelType::WORKER_OBJECT_LOCATIONS_CHANNEL},
      RayConfig::instance().max_command_batch_size(),
      /*get_client=*/
      [this](const rpc::Address &address) {
        return worker_rpc_pool_.GetOrConnect(address);
      },
      &main_service);
  auto object_directory = std::make_unique<OwnershipBasedObjectDirectory>(
      main_service,
      gcs_client_,
      core_worker_subscriber.get(),
      &worker_rpc_pool_,
      [this](const ObjectID &obj_id, const ErrorType &error_type) {
        rpc::ObjectReference ref;
        ref.set_object_id(obj_id.Binary());
        this->node_manager_->MarkObjectsAsFailed(error_type, {ref}, JobID::Nil());
      });
  auto object_manager = std::make_unique<ObjectManager>(
      main_service,
      self_node_id,
      object_manager_config,
      object_directory.get(),
      /*restore_spilled_object=*/
      [this](const ObjectID &object_id,
             int64_t object_size,
             const std::string &object_url,
             std::function<void(const ray::Status &)> callback) {
        this->node_manager_->GetLocalObjectManager().AsyncRestoreSpilledObject(
            object_id, object_size, object_url, std::move(callback));
      },
      /*get_spilled_object_url=*/
      [this](const ObjectID &object_id) {
        return this->node_manager_->GetLocalObjectManager().GetLocalSpilledObjectURL(
            object_id);
      },
      /*spill_objects_callback=*/
      [this, &main_service]() {
        // This callback is called from the plasma store thread.
        // NOTE: It means the local object manager should be thread-safe.
        main_service.post(
            [this]() {
              this->node_manager_->GetLocalObjectManager().SpillObjectUptoMaxThroughput();
            },
            "NodeManager.SpillObjects");
        return this->node_manager_->GetLocalObjectManager().IsSpillingInProgress();
      },
      /*object_store_full_callback=*/
      [this, &main_service]() {
        // Post on the node manager's event loop since this
        // callback is called from the plasma store thread.
        // This will help keep node manager lock-less.
        main_service.post([this]() { this->node_manager_->TriggerGlobalGC(); },
                          "NodeManager.GlobalGC");
      },
      /*add_object_callback=*/
      [this](const ObjectInfo &object_info) {
        this->node_manager_->HandleObjectLocal(object_info);
      },
      /*delete_object_callback=*/
      [this](const ObjectID &object_id) {
        this->node_manager_->HandleObjectMissing(object_id);
      },
      /*pin_object=*/
      [this](const ObjectID &object_id) {
        std::vector<ObjectID> object_ids = {object_id};
        std::vector<std::unique_ptr<RayObject>> results;
        std::unique_ptr<RayObject> result;
        if (this->node_manager_->GetObjectsFromPlasma(object_ids, &results) &&
            results.size() > 0) {
          result = std::move(results[0]);
        }
        return result;
      },
      /*fail_pull_request=*/
      [this](const ObjectID &object_id, rpc::ErrorType error_type) {
        rpc::ObjectReference ref;
        ref.set_object_id(object_id.Binary());
        this->node_manager_->MarkObjectsAsFailed(error_type, {ref}, JobID::Nil());
      });
  node_manager_ = std::make_unique<NodeManager>(main_service,
                                                self_node_id,
                                                node_name,
                                                node_manager_config,
                                                gcs_client_,
                                                client_call_manager_,
                                                worker_rpc_pool_,
                                                std::move(core_worker_subscriber),
                                                std::move(object_directory),
                                                std::move(object_manager),
                                                std::make_unique<plasma::PlasmaClient>(),
                                                std::move(shutdown_raylet_gracefully));

  SetCloseOnExec(acceptor_);
  self_node_info_.set_node_id(self_node_id_.Binary());
  self_node_info_.set_state(GcsNodeInfo::ALIVE);
  self_node_info_.set_node_manager_address(node_ip_address);
  self_node_info_.set_node_name(node_name);
  self_node_info_.set_raylet_socket_name(socket_name);
  self_node_info_.set_object_store_socket_name(object_manager_config.store_socket_name);
  self_node_info_.set_object_manager_port(node_manager_->GetObjectManagerPort());
  self_node_info_.set_node_manager_port(node_manager_->GetServerPort());
  self_node_info_.set_node_manager_hostname(boost::asio::ip::host_name());
  self_node_info_.set_metrics_export_port(metrics_export_port);
  self_node_info_.set_runtime_env_agent_port(node_manager_config.runtime_env_agent_port);
  self_node_info_.mutable_state_snapshot()->set_state(NodeSnapshot::ACTIVE);
  auto resource_map = node_manager_config.resource_config.GetResourceMap();
  self_node_info_.mutable_resources_total()->insert(resource_map.begin(),
                                                    resource_map.end());
  self_node_info_.set_start_time_ms(current_sys_time_ms());
  self_node_info_.set_is_head_node(is_head_node);
  self_node_info_.mutable_labels()->insert(node_manager_config.labels.begin(),
                                           node_manager_config.labels.end());

  // Setting up autoscaler related fields from ENV
  auto instance_id = std::getenv(kNodeCloudInstanceIdEnv);
  self_node_info_.set_instance_id(instance_id ? instance_id : "");
  auto cloud_node_type_name = std::getenv(kNodeTypeNameEnv);
  self_node_info_.set_node_type_name(cloud_node_type_name ? cloud_node_type_name : "");
  auto instance_type_name = std::getenv(kNodeCloudInstanceTypeNameEnv);
  self_node_info_.set_instance_type_name(instance_type_name ? instance_type_name : "");
}

Raylet::~Raylet() {}

void Raylet::Start() {
  RAY_CHECK_OK(RegisterGcs());

  // Start listening for clients.
  DoAccept();
}

void Raylet::UnregisterSelf(const rpc::NodeDeathInfo &node_death_info,
                            std::function<void()> unregister_done_callback) {
  gcs_client_->Nodes().UnregisterSelf(node_death_info, unregister_done_callback);
}

void Raylet::Stop() {
  node_manager_->Stop();
  acceptor_.close();
}

ray::Status Raylet::RegisterGcs() {
  auto register_callback = [this](const Status &status) {
    RAY_CHECK_OK(status);
    RAY_LOG(INFO) << "Raylet of id, " << self_node_id_
                  << " started. Raylet consists of node_manager and object_manager."
                  << " node_manager address: " << self_node_info_.node_manager_address()
                  << ":" << self_node_info_.node_manager_port()
                  << " object_manager address: " << self_node_info_.node_manager_address()
                  << ":" << self_node_info_.object_manager_port()
                  << " hostname: " << self_node_info_.node_manager_hostname();
    RAY_CHECK_OK(node_manager_->RegisterGcs());
  };

  RAY_RETURN_NOT_OK(
      gcs_client_->Nodes().RegisterSelf(self_node_info_, register_callback));
  return Status::OK();
}

void Raylet::DoAccept() {
  acceptor_.async_accept(
      socket_,
      boost::bind(&Raylet::HandleAccept, this, boost::asio::placeholders::error));
}

void Raylet::HandleAccept(const boost::system::error_code &error) {
  if (!error) {
    ConnectionErrorHandler error_handler = [this](
                                               std::shared_ptr<ClientConnection> client,
                                               const boost::system::error_code &error) {
      node_manager_->HandleClientConnectionError(client, error);
    };

    MessageHandler message_handler = [this](std::shared_ptr<ClientConnection> client,
                                            int64_t message_type,
                                            const std::vector<uint8_t> &message) {
      node_manager_->ProcessClientMessage(client, message_type, message.data());
    };

    // Accept a new local client and dispatch it to the node manager.
    auto conn = ClientConnection::Create(message_handler,
                                         error_handler,
                                         std::move(socket_),
                                         "worker",
                                         node_manager_message_enum);

    // Begin processing messages. The message handler above is expected to call this to
    // continue processing messages.
    conn->ProcessMessages();
  } else {
    RAY_LOG(ERROR) << "Raylet failed to accept new connection: " << error.message();
    if (error == boost::asio::error::operation_aborted) {
      // The server is being destroyed. Don't continue accepting connections.
      return;
    }
  };

  // We're ready to accept another client.
  DoAccept();
}

}  // namespace raylet

}  // namespace ray
