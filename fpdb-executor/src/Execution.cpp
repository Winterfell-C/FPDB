//
// Created by Yifei Yang on 11/23/21.
//

#include <fpdb/executor/Execution.h>
#include <fpdb/executor/ModeCtrl.h>
#include <fpdb/executor/physical/POpContext.h>
#include <fpdb/executor/physical/POpDirectoryEntry.h>
#include <fpdb/executor/physical/POpConnection.h>
#include <fpdb/executor/physical/POpRelationshipType.h>
#include <fpdb/executor/physical/filter/FilterPOp.h>
#include <fpdb/executor/serialization/POpSerializer.h>
#include <caf/io/all.hpp>
#include <graphviz/gvc.h>

namespace fpdb::executor {

Execution::Execution(long queryId, 
                     const shared_ptr<::caf::actor_system> &actorSystem,
                     const vector<::caf::node_id> &nodes,
                     const ::caf::actor &segmentCacheActor,
                     const shared_ptr<PhysicalPlan> &physicalPlan,
                     bool isDistributed) :
  queryId_(queryId),
  actorSystem_(actorSystem),
  nodes_(nodes),
  segmentCacheActor_(segmentCacheActor),
  physicalPlan_(physicalPlan),
  isDistributed_(isDistributed) {
  rootActor_ = make_shared<::caf::scoped_actor>(*actorSystem_);
}

Execution::~Execution() {
  close();
}

shared_ptr<TupleSet> Execution::execute() {
  boot();
  start();
  join();
  return legacyCollateOperator_->tuples();
}

void Execution::boot() {
  startTime_ = chrono::steady_clock::now();
  ModeCtrl ctrl(true);//controlled by external file

  // Tell segment cache actor that new query comes
  if (segmentCacheActor_) {
    (*rootActor_)->anon_send(segmentCacheActor_, NewQueryAtom_v);
  }

  // Set query id, and add physical operators to operator directory
  for (const auto &op: physicalPlan_->getPhysicalOps()) {
    assert(op);
    op->setQueryId(queryId_);
    auto result = opDirectory_.insert(POpDirectoryEntry(op, nullptr, false));
    if (!result.has_value()) {
      throw runtime_error(result.error());
    }
  }

  // Spawn actors locally/remotely according to nodeId assigned
  for (auto &element: opDirectory_) {
    auto op = element.second.getDef();
    auto ctx = make_shared<POpContext>(*rootActor_, segmentCacheActor_);
    op->create(ctx);

    // Spawn collate at the coordinator
    if (op->getType() == POpType::COLLATE) {
      legacyCollateOperator_ = static_pointer_cast<physical::collate::CollatePOp>(op);
      element.second.setActorHandle(localSpawn(op));
    } else {
      // Check if operator name starts with 'Filter_onScan'
      auto allowedOpTypes = {
        POpType::FILE_SCAN,
        POpType::FILTER,
        // POpType::SHUFFLE,
        POpType::AGGREGATE,
        POpType::SEND
      };
      bool spawnOnRemote = ctrl.canPushdown(op->getType());
      // bool spawnOnRemote = op->spawnOnRemote() && find(allowedOpTypes.begin(), allowedOpTypes.end(), op->getType()) != allowedOpTypes.end();
      if(isDistributed_ && spawnOnRemote) {
        element.second.setActorHandle(remoteSpawn(op, op->getNodeId()));
      } else {
        element.second.setActorHandle(localSpawn(op));
      }
    }
  }
}

::caf::actor Execution::localSpawn(const shared_ptr<PhysicalOp> &op) {
  // TODO: After POpActor2 is generally finished we may use this
//  if(op->getType() == "FileScan"){
//    auto fileScanOp = static_pointer_cast<FileScan>(op);
//    auto actorHandle = operatorManager_.lock()->getActorSystem()->spawn(FileScanFunctor,
//                                                                        fileScanOp->name(),
//                                                                        fileScanOp->getKernel()->getPath(),
//                                                                        fileScanOp->getKernel()->getFileType().value(),
//                                                                        fileScanOp->getColumnNames(),
//                                                                        fileScanOp->getKernel()->getStartPos(),
//                                                                        fileScanOp->getKernel()->getFinishPos(),
//                                                                        fileScanOp->getQueryId(),
//                                                                        *rootActor_,
//                                                                        operatorManager_.lock()->getSegmentCacheActor(),
//                                                                        fileScanOp->isScanOnStart()
//    );
//    if (!actorHandle)
//      throw runtime_error(fmt::format("Failed to spawn operator actor '{}'", op->name()));
//    element.second.setActorHandle(::caf::actor_cast<::caf::actor>(actorHandle));
//  }
//  else if(op->getType() == "Collate"){
//    legacyCollateOperator_ = static_pointer_cast<Collate>(op);
//    collateActorHandle_  = operatorManager_.lock()->getActorSystem()->spawn(CollateFunctor,
//                                        legacyCollateOperator_->name(),
//                                        legacyCollateOperator_->getQueryId(),
//                                      *rootActor_,
//                                      operatorManager_.lock()->getSegmentCacheActor()
//    );
//    if (!collateActorHandle_)
//    throw runtime_error(fmt::format("Failed to spawn operator actor '{}'", op->name()));
//    element.second.setActorHandle(::caf::actor_cast<::caf::actor>(collateActorHandle_));
//  }

  // Need s3 operators to be "detached" to not block others while loading data.
  // Don't run more S3Get requests in parallel than # cores, earlier testing showed this did not help as S3Get
  // already utilizes the full network bandwidth with #cores requests whereas S3Select does not when
  // selectivity is low.
  if (op->getType() == POpType::S3_SELECT || op->getType() == POpType::S3_GET) {
    auto actorHandle = actorSystem_->spawn<POpActor, detached>(op);
    if (!actorHandle)
      throw runtime_error(fmt::format("Failed to spawn operator actor '{}'", op->name()));
    return ::caf::actor_cast<::caf::actor>(actorHandle);
  }

  else {
    auto actorHandle = actorSystem_->spawn<POpActor>(op);
    if (!actorHandle)
      throw runtime_error(fmt::format("Failed to spawn operator actor '{}'", op->name()));
    return ::caf::actor_cast<::caf::actor>(actorHandle);
  }
}

::caf::actor Execution::remoteSpawn(const shared_ptr<PhysicalOp> &op, int nodeId) {
  std::cout<<"Remote Spawn Op: "<<op->name()<<std::endl;
  auto remoteSpawnTout = std::chrono::seconds(10);
  auto args = make_message(op);

  // Need s3 operators to be "detached" to not block others while loading data.
  // Don't run more S3Get requests in parallel than # cores, earlier testing showed this did not help as S3Get
  // already utilizes the full network bandwidth with #cores requests whereas S3Select does not when
  // selectivity is low.
  auto actorTypeName = (op->getType() == POpType::S3_SELECT || op->getType() == POpType::S3_GET) ?
          "POpActor-detached" : "POpActor";

  auto expectedActorHandle = actorSystem_->middleman().remote_spawn<::caf::actor>(nodes_[nodeId],
                                                                                  actorTypeName,
                                                                                  args,
                                                                                  remoteSpawnTout);
  if (!expectedActorHandle) {
    throw std::runtime_error(fmt::format("Failed to remote-spawn operator actor '{}': {}",
                                         op->name(),
                                         to_string(expectedActorHandle.error())));
  }
  return *expectedActorHandle;
}

void Execution::start() {
  // Mark all the operators as incomplete
  opDirectory_.setIncomplete();

  // Connect the actors
  for (const auto &element: opDirectory_) {
    auto entry = element.second;

    vector<POpConnection> opConnections;
    for (const auto &producer: element.second.getDef()->producers()) {
      auto producerHandle = opDirectory_.get(producer).value().getActorHandle();
      opConnections.emplace_back(producer, producerHandle, POpRelationshipType::Producer);
    }
    for (const auto &consumer: element.second.getDef()->consumers()) {
      auto consumerHandle = opDirectory_.get(consumer).value().getActorHandle();
      opConnections.emplace_back(consumer, consumerHandle, POpRelationshipType::Consumer);
    }

    auto cm = make_shared<message::ConnectMessage>(opConnections, ExecutionRootActorName);
    (*rootActor_)->anon_send(element.second.getActorHandle(), Envelope(cm));
  }

  // Start the actors
  for (const auto &element: opDirectory_) {
    auto entry = element.second;
    auto sm = make_shared<message::StartMessage>(ExecutionRootActorName);
    (*rootActor_)->anon_send(element.second.getActorHandle(), Envelope(sm));
  }
}

void Execution::join() {
  SPDLOG_DEBUG("Waiting for all operators to complete");

  auto handle_err = [&](const ::caf::error &err) {
    throw runtime_error(to_string(err));
  };

  bool allComplete = false;
  (*rootActor_)->receive_while([&] { return !allComplete; })(
          [&](const Envelope &e) {
            const auto &msg = e.message();
            SPDLOG_DEBUG("Query root actor received message  |  query: '{}', messageKind: '{}', from: '{}'",
                         queryId_, msg.getTypeString(), msg.sender());

            auto errAct = [&](const std::string &errMsg) {
              allComplete = true;
              close();
              throw runtime_error(errMsg);
            };

            switch (msg.type()) {
              case MessageType::COMPLETE: {
                this->opDirectory_.setComplete(msg.sender())
                        .map_error(errAct);
                allComplete = this->opDirectory_.allComplete();
                break;
              }
              case MessageType::ERROR: {
                errAct(fmt::format("ERROR: {}, from {}", ((ErrorMessage &) msg).getContent(), msg.sender()));
              }
              default: {
                errAct(fmt::format("Invalid message type sent to the root actor: {}, from {}", msg.getTypeString(), msg.sender()));
              }
            }

          },
          handle_err);

  // TODO: After POpActor2 is generally finished we may use this
//  (*rootActor_)->request(collateActorHandle_, ::caf::infinite, GetTupleSetAtom::value).receive(
//	  [&](const tl::expected<shared_ptr<TupleSet>, string> &expectedTupleSet) {
//		legacyCollateOperator_->setTuples(expectedTupleSet.value());
//	  },
//	  [&](const ::caf::error&  error){
//		throw runtime_error(to_string(error));
//	  });

  stopTime_ = chrono::steady_clock::now();
}

void Execution::close() {
  if(rootActor_ != nullptr){
    for (const auto &element: opDirectory_) {
      (*rootActor_)->send_exit(element.second.getActorHandle(), ::caf::exit_reason::user_shutdown);
    }
    rootActor_.reset();
  }
  legacyCollateOperator_.reset();
}

void Execution::write_graph(const string &file) {
  auto gvc = gvContext();

  auto graph = agopen(const_cast<char *>(string("Execution Plan").c_str()), Agstrictdirected, nullptr);

  // Init attributes
  agattr(graph, AGNODE, const_cast<char *>("fixedsize"), const_cast<char *>("false"));
  agattr(graph, AGNODE, const_cast<char *>("shape"), const_cast<char *>("ellipse"));
  agattr(graph, AGNODE, const_cast<char *>("label"), const_cast<char *>("<not set>"));
  agattr(graph, AGNODE, const_cast<char *>("fontname"), const_cast<char *>("Arial"));
  agattr(graph, AGNODE, const_cast<char *>("fontsize"), const_cast<char *>("8"));

  // Add all the nodes
  for (const auto &op: this->opDirectory_) {
    string nodeName = op.second.getDef()->name();
    auto node = agnode(graph, const_cast<char *>(nodeName.c_str()), true);

    agset(node, const_cast<char *>("shape"), const_cast<char *>("plaintext"));

    string nodeLabel = "<table border='1' cellborder='0' cellpadding='5'>"
                            "<tr><td align='left'><b>" + op.second.getDef()->getTypeString() + "</b></td></tr>"
                                                                                         "<tr><td align='left'>"
                            + op.second.getDef()->name() + "</td></tr>"
                                                           "</table>";
    char *htmlNodeLabel = agstrdup_html(graph, const_cast<char *>(nodeLabel.c_str()));
    agset(node, const_cast<char *>("label"), htmlNodeLabel);
    agstrfree(graph, htmlNodeLabel);
  }

  // Add all the edges
  for (const auto &op: this->opDirectory_) {
    auto opNode = agfindnode(graph, (char *)(op.second.getDef()->name().c_str()));
    for (const auto &c: op.second.getDef()->consumers()) {
      auto consumerOpNode = agfindnode(graph, (char *)(c.c_str()));
      agedge(graph, opNode, consumerOpNode, const_cast<char *>(string("Edge").c_str()), true);
    }
  }

  const filesystem::path &path = filesystem::path(file);
  if (!filesystem::exists(path.parent_path())) {
    throw runtime_error("Could not open file '" + file + "' for writing. Parent directory does not exist");
  } else {
    FILE *outFile = fopen(file.c_str(), "w");
    if (outFile == nullptr) {
      throw runtime_error("Could not open file '" + file + "' for writing. Errno: " + to_string(errno));
    }

    gvLayout(gvc, graph, "dot");
    gvRender(gvc, graph, "svg", outFile);

    fclose(outFile);

    gvFreeLayout(gvc, graph);
    agclose(graph);
    gvFreeContext(gvc);
  }
}

long Execution::getQueryId() const {
  return queryId_;
}

long Execution::getElapsedTime() {
  if (startTime_.time_since_epoch().count() == 0)
    throw runtime_error("Execution time unavailable, query has not been started");
  if (stopTime_.time_since_epoch().count() == 0)
    throw runtime_error("Execution time unavailable, query has not been stopped");

  return chrono::duration_cast<chrono::nanoseconds>(stopTime_ - startTime_).count();
}

shared_ptr<PhysicalOp> Execution::getPhysicalOp(const string &name) {
  const auto expectedEntry = opDirectory_.get(name);
  if (!expectedEntry.has_value()) {
    throw runtime_error(expectedEntry.error());
  }
  return expectedEntry.value().getDef();
}

physical::s3::S3SelectScanStats Execution::getAggregateS3SelectScanStats() {
  physical::s3::S3SelectScanStats aggregateS3SelectScanStats = {0, 0, 0, 0 , 0, 0 ,0, 0}; // initialize all fields to 0
  for (const auto &entry: opDirectory_) {
    const auto op = entry.second.getDef();
    if (op->getType() == POpType::S3_GET || op->getType() == POpType::S3_SELECT) {
      auto s3SelectScanOp = static_pointer_cast<physical::s3::S3SelectScanAbstractPOp>(op);
      physical::s3::S3SelectScanStats currentS3SelectScanStats = s3SelectScanOp->getS3SelectScanStats();

      // Add these s3SelectScanStats to our aggregate stats
      aggregateS3SelectScanStats.processedBytes += currentS3SelectScanStats.processedBytes;
      aggregateS3SelectScanStats.returnedBytes += currentS3SelectScanStats.returnedBytes;
      aggregateS3SelectScanStats.outputBytes += currentS3SelectScanStats.outputBytes;
      aggregateS3SelectScanStats.numRequests += currentS3SelectScanStats.numRequests;
      aggregateS3SelectScanStats.getTransferTimeNS += currentS3SelectScanStats.getTransferTimeNS;
      aggregateS3SelectScanStats.getConvertTimeNS += currentS3SelectScanStats.getConvertTimeNS;
      aggregateS3SelectScanStats.selectTransferTimeNS += currentS3SelectScanStats.selectTransferTimeNS;
      aggregateS3SelectScanStats.selectConvertTimeNS += currentS3SelectScanStats.selectConvertTimeNS;
    }
  }
  return aggregateS3SelectScanStats;
}

std::tuple<size_t, size_t, size_t> Execution::getFilterTimeNSInputOutputBytes() {
  size_t timeNS = 0;
  size_t inputBytes = 0;
  size_t outputBytes = 0;
  for (const auto &entry: opDirectory_) {
    const auto op = entry.second.getDef();
    if (op->getType() == POpType::FILTER) {
      auto filterOp = static_pointer_cast<filter::FilterPOp>(entry.second.getDef());

      timeNS += filterOp->getFilterTimeNS();
      inputBytes += filterOp->getFilterInputBytes();
      outputBytes += filterOp->getFilterOutputBytes();
    }
  }
  return {timeNS, inputBytes, outputBytes};
}

string Execution::showMetrics(bool showOpTimes, bool showScanMetrics) {
  stringstream ss;
  if (!showOpTimes && !showScanMetrics) {
    return ss.str();
  }

  ss << endl;
  ss << "Metrics |" << endl << endl;
  long totalProcessingTime = 0;
  for (auto &entry : opDirectory_) {
    (*rootActor_)->request(entry.second.getActorHandle(), ::caf::infinite, GetProcessingTimeAtom_v).receive(
            [&](long processingTime) {
              totalProcessingTime += processingTime;
            },
            [&](const ::caf::error&  error){
              throw runtime_error(to_string(error));
            });
  }

  if (showOpTimes) {
    auto totalExecutionTime = getElapsedTime();
    stringstream formattedExecutionTime;
    formattedExecutionTime << totalExecutionTime << " \u33B1" << " (" << ((double) totalExecutionTime / 1000000000.0)
                           << " secs)";
    ss << left << setw(60) << "Total Execution Time ";
    ss << left << setw(60) << formattedExecutionTime.str();
    ss << endl;
    ss << endl;

    ss << left << setw(120) << "Operator Execution Times" << endl;
    ss << setfill(' ');

    ss << left << setw(120) << setfill('-') << "" << endl;
    ss << setfill(' ');

    ss << left << setw(60) << "Operator";
    ss << left << setw(40) << "Execution Time";
    ss << left << setw(20) << "% Total Time";
    ss << endl;

    ss << left << setw(120) << setfill('-') << "" << endl;
    ss << setfill(' ');

    map<string, size_t> opTypeToRuntime;

    for (auto &entry : opDirectory_) {
      auto operatorName = entry.first;

      long processingTime;
      (*rootActor_)->request(entry.second.getActorHandle(), ::caf::infinite, GetProcessingTimeAtom_v).receive(
              [&](long time) {
                processingTime = time;
              },
              [&](const ::caf::error &error) {
                throw runtime_error(to_string(error));
              });

      auto op = entry.second.getDef();
      string typeString = op->getTypeString();
      if (opTypeToRuntime.find(typeString) == opTypeToRuntime.end()) {
        opTypeToRuntime.emplace(typeString, processingTime);
      } else {
        opTypeToRuntime[typeString] = opTypeToRuntime[typeString] + processingTime;
      }

      auto processingFraction = (double) processingTime / (double) totalProcessingTime;
      stringstream formattedProcessingTime;
      formattedProcessingTime << processingTime << " \u33B1" << " (" << ((double) processingTime / 1000000000.0)
                              << " secs)";
      stringstream formattedProcessingPercentage;
      formattedProcessingPercentage << (processingFraction * 100.0);
      ss << left << setw(60) << operatorName;
      ss << left << setw(40) << formattedProcessingTime.str();
      ss << left << setw(20) << formattedProcessingPercentage.str();
      ss << endl;
    }

    ss << left << setw(120) << setfill('-') << "" << endl;
    ss << setfill(' ');
    ss << endl;

    for (auto const &opTime : opTypeToRuntime) {
      stringstream formattedOpTime;
      formattedOpTime << ((double) opTime.second / 1000000000.0) << " secs";
      ss << left << setw(60) << opTime.first;
      ss << left << setw(40) << formattedOpTime.str();
      ss << left << setw(20) << ((double) opTime.second / (double) totalProcessingTime) * 100.0;
      ss << endl;
    }

    ss << left << setw(120) << setfill('-') << "" << endl;
    ss << setfill(' ');

    stringstream formattedProcessingTime;
    formattedProcessingTime << totalProcessingTime << " \u33B1" << " (" << ((double) totalProcessingTime / 1000000000.0)
                            << " secs)";
    ss << left << setw(60) << "Total ";
    ss << left << setw(40) << formattedProcessingTime.str();
    ss << left << setw(20) << "100.0";
    ss << endl;
    ss << endl;
  }

  if (showScanMetrics) {
    physical::s3::S3SelectScanStats s3SelectScanStats = getAggregateS3SelectScanStats();

    stringstream formattedProcessedBytes;
    formattedProcessedBytes << s3SelectScanStats.processedBytes << " B" << " ("
                            << ((double) s3SelectScanStats.processedBytes / 1024.0 / 1024.0 / 1024.0) << " GB)";
    stringstream formattedReturnedBytes;
    formattedReturnedBytes << s3SelectScanStats.returnedBytes << " B" << " ("
                           << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0 / 1024.0) << " GB)";
    stringstream formattedArrowConvertedBytes;
    formattedArrowConvertedBytes << s3SelectScanStats.outputBytes << " B" << " ("
                                 << ((double) s3SelectScanStats.outputBytes / 1024.0 / 1024.0 / 1024.0) << " GB)";

    stringstream formattedConversionOutputRate;
    if (s3SelectScanStats.getConvertTimeNS + s3SelectScanStats.selectConvertTimeNS > 0) {
      formattedConversionOutputRate << ((double) s3SelectScanStats.outputBytes / 1024.0 / 1024.0) /
                                       ((double) (s3SelectScanStats.getConvertTimeNS +
                                                  s3SelectScanStats.selectConvertTimeNS) / 1.0e9) << " MB/s/req";
    } else {
      formattedConversionOutputRate << "NA";
    }

    stringstream formattedStorageFormatToArrowSizeX;
    if (s3SelectScanStats.outputBytes > 0) {
      formattedStorageFormatToArrowSizeX
              << (double) s3SelectScanStats.returnedBytes / (double) s3SelectScanStats.outputBytes << "x";
    } else {
      formattedStorageFormatToArrowSizeX << "NA";
    }
    ss << left << setw(60) << "Processed Bytes";
    ss << left << setw(60) << formattedProcessedBytes.str();
    ss << endl;
    ss << left << setw(60) << "Returned Bytes";
    ss << left << setw(60) << formattedReturnedBytes.str();
    ss << endl;
    ss << left << setw(60) << "Arrow Converted Bytes";
    ss << left << setw(60) << formattedArrowConvertedBytes.str();
    ss << endl;
    ss << left << setw(60) << "Conversion Output Rate";
    ss << left << setw(60) << formattedConversionOutputRate.str();
    ss << endl;
    ss << left << setw(60) << "Storage/Compute Data Ratio";
    ss << left << setw(60) << formattedStorageFormatToArrowSizeX.str();
    ss << endl;

    stringstream formattedGetTransferRate;
    stringstream formattedGetConvertRate;
    stringstream formattedGetTransferConvertRate;
    if (s3SelectScanStats.getTransferTimeNS > 0 && s3SelectScanStats.getConvertTimeNS > 0) {
      formattedGetTransferRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                  ((double) s3SelectScanStats.getTransferTimeNS / 1.0e9) << " MB/s/req";
      formattedGetConvertRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                 ((double) s3SelectScanStats.getConvertTimeNS / 1.0e9) << " MB/s/req";
      formattedGetTransferConvertRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                         (((double) s3SelectScanStats.getTransferTimeNS +
                                         (double) s3SelectScanStats.getConvertTimeNS) / 1.0e9) << " MB/s/req";
    } else {
      formattedGetTransferRate << "NA";
      formattedGetConvertRate << "NA";
      formattedGetTransferConvertRate << "NA";
    }
    // Caf actor framework seems to converge #workers -> # cores for non detached workers, and each worker runs to
    // completion. This should approximate to per core rates rather than just per request, as one request maps to a core
    ss << left << setw(60) << "S3 GET Data Transfer Rate";
    ss << left << setw(60) << formattedGetTransferRate.str();
    ss << endl;
    ss << left << setw(60) << "S3 GET Data Convert rate";
    ss << left << setw(60) << formattedGetConvertRate.str();
    ss << endl;
    ss << left << setw(60) << "S3 GET Data Transfer + Convert rate";
    ss << left << setw(60) << formattedGetTransferConvertRate.str();
    ss << endl;

    stringstream formattedSelectTransferRate;
    stringstream formattedSelectConvertRate;
    stringstream formattedSelectTransferConvertRate;
    if (s3SelectScanStats.selectTransferTimeNS > 0 && s3SelectScanStats.selectConvertTimeNS > 0) {
      formattedSelectTransferRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                     ((double) (s3SelectScanStats.selectTransferTimeNS) / 1.0e9) << " MB/s/req";
      formattedSelectConvertRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                    ((double) s3SelectScanStats.selectConvertTimeNS / 1.0e9) << " MB/s/req";
      formattedSelectTransferConvertRate << ((double) s3SelectScanStats.returnedBytes / 1024.0 / 1024.0) /
                                            ((double) (s3SelectScanStats.selectTransferTimeNS +
                                                       s3SelectScanStats.selectConvertTimeNS) / 1.0e9) << " MB/s/req";
    } else {
      formattedSelectTransferRate << "NA";
      formattedSelectConvertRate << "NA";
      formattedSelectTransferConvertRate << "NA";
    }

    // FIXME: This only works if the query is entirely pushdown (it only uses S3 Select and never GET), as the bytes
    //        transferred is grouped together for select and get requests, so they are not differentiated
    //        If we decide to eventually allow GET and Select requests to be in the same query for a mode we will need
    //        to have a way to differentiate this (this isn't the case for now though so it is fine)
    //        Also note that some storage backends such as Airmettle don't tell use processedBytes, in which case
    //        we estimate this value with the select range.
    stringstream formattedS3SelectSelectivityPercent;
    if (s3SelectScanStats.returnedBytes > 0 && s3SelectScanStats.processedBytes &&
        s3SelectScanStats.selectTransferTimeNS > 0 && s3SelectScanStats.getConvertTimeNS == 0) {
      formattedS3SelectSelectivityPercent << (double) s3SelectScanStats.returnedBytes /
                                             (double) s3SelectScanStats.processedBytes * 100 << "%";
    } else {
      formattedS3SelectSelectivityPercent << "NA";
    }
    ss << left << setw(60) << "Appx S3 Select Data Transfer Rate";
    ss << left << setw(60) << formattedSelectTransferRate.str();
    ss << endl;
    ss << left << setw(60) << "S3 Select Data Convert rate";
    ss << left << setw(60) << formattedSelectConvertRate.str();
    ss << endl;
    ss << left << setw(60) << "S3 Select Data Transfer + Convert rate";
    ss << left << setw(60) << formattedSelectTransferConvertRate.str();
    ss << endl;
    ss << left << setw(60) << "S3 Selectivity";
    ss << left << setw(60) << formattedS3SelectSelectivityPercent.str();
    ss << endl;

    auto filterTimeNSInputOutputBytes = getFilterTimeNSInputOutputBytes();
    size_t filterTimeNS = get<0>(filterTimeNSInputOutputBytes);
    size_t filterInputBytes = get<1>(filterTimeNSInputOutputBytes);
    size_t filterOutputBytes = get<2>(filterTimeNSInputOutputBytes);
    stringstream formattedLocalFilterRateGBs;
    stringstream formattedLocalFilterGB;
    stringstream formattedLocalFilterSelectivity;
    if (filterTimeNS > 0) {
      double filterGB = ((double) filterInputBytes / 1024.0 / 1024.0 / 1024.0);
      formattedLocalFilterRateGBs << filterGB / ((double) filterTimeNS / 1.0e9) << " GB/s/req";
      formattedLocalFilterGB << filterGB << " GB";
      formattedLocalFilterSelectivity << ((double) filterOutputBytes / (double) filterInputBytes) * 100 << "%";
    } else {
      formattedLocalFilterRateGBs << "NA";
      formattedLocalFilterGB << "NA";
      formattedLocalFilterSelectivity << "NA";
    }
    // Caf actor framework seems to converge #workers -> # cores, and each worker runs to completion
    // so this should approximate to per core rates rather than just per request, as one request maps to a core
    ss << left << setw(60) << "Local filter rate";
    ss << left << setw(60) << formattedLocalFilterRateGBs.str();
    ss << endl;
    ss << left << setw(60) << "Local filter bytes";
    ss << left << setw(60) << formattedLocalFilterGB.str();
    ss << endl;
    ss << left << setw(60) << "Local filter selectivity (bytes)";
    ss << left << setw(60) << formattedLocalFilterSelectivity.str();
    ss << endl;
    ss << endl;
  }

  return ss.str();
}

}
