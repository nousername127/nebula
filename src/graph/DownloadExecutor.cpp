/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/StatusOr.h"
#include "http/HttpClient.h"
#include "graph/DownloadExecutor.h"
#include "process/ProcessUtils.h"
#include "webservice/Common.h"

#include <folly/executors/Async.h>
#include <folly/futures/Future.h>
#include <folly/executors/ThreadedExecutor.h>

namespace nebula {
namespace graph {

DownloadExecutor::DownloadExecutor(Sentence *sentence,
                                   ExecutionContext *ectx) : Executor(ectx) {
    sentence_ = static_cast<DownloadSentence*>(sentence);
}

Status DownloadExecutor::prepare() {
    return Status::OK();
}

void DownloadExecutor::execute() {
    auto status = checkIfGraphSpaceChosen();
    if (!status.ok()) {
        DCHECK(onError_);
        onError_(std::move(status));
        return;
    }

    auto *mc = ectx()->getMetaClient();
    auto  addresses = mc->getAddresses();
    auto  metaHost = network::NetworkUtils::intToIPv4(addresses[0].first);
    auto  spaceId = ectx()->rctx()->session()->space();
    auto *hdfsHost  = sentence_->host();
    auto  hdfsPort  = sentence_->port();
    auto *hdfsPath  = sentence_->path();
    if (hdfsHost == nullptr || hdfsPort == 0 || hdfsPath == nullptr) {
        LOG(ERROR) << "URL Parse Failed";
        resp_ = std::make_unique<cpp2::ExecutionResponse>();
        onError_(Status::Error("URL Parse Failed"));
        return;
    }

    auto func = [metaHost, hdfsHost, hdfsPort, hdfsPath, spaceId]() {
        static const char *tmp = "http://%s:%d/%s?host=%s&port=%d&path=%s&space=%d";
        auto url = folly::stringPrintf(tmp, metaHost.c_str(), FLAGS_ws_meta_http_port,
                                       "download-dispatch", hdfsHost->c_str(),
                                       hdfsPort, hdfsPath->c_str(), spaceId);
        auto result = http::HttpClient::get(url);
        if (result.ok() && result.value() == "SSTFile dispatch successfully") {
            LOG(INFO) << "Download Successfully";
            return true;
        } else {
            LOG(ERROR) << "Download Failed ";
            return false;
        }
    };
    auto future = folly::async(func);

    auto *runner = ectx()->rctx()->runner();

    auto cb = [this] (auto &&resp) {
        if (!resp) {
            DCHECK(onError_);
            onError_(Status::Error("Download Failed"));
            return;
        }
        resp_ = std::make_unique<cpp2::ExecutionResponse>();
        DCHECK(onFinish_);
        onFinish_();
    };

    auto error = [this] (auto &&e) {
        LOG(ERROR) << "Exception caught: " << e.what();
        DCHECK(onError_);
        onError_(Status::Error("Internal error"));
        return;
    };

    std::move(future).via(runner).thenValue(cb).thenError(error);
}

void DownloadExecutor::setupResponse(cpp2::ExecutionResponse &resp) {
    resp = std::move(*resp_);
}

}   // namespace graph
}   // namespace nebula

