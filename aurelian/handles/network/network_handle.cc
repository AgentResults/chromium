// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/network/network_handle.h"

#include <mutex>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_util.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace aurelian {

InterceptRule::InterceptRule() = default;
InterceptRule::~InterceptRule() = default;
InterceptRule::InterceptRule(const InterceptRule&) = default;
InterceptRule& InterceptRule::operator=(const InterceptRule&) = default;
InterceptRule::InterceptRule(InterceptRule&&) = default;
InterceptRule& InterceptRule::operator=(InterceptRule&&) = default;

namespace {

// Thread-safe rule storage (accessed from IO thread throttle + UI thread API).
std::mutex& RuleMutex() {
  static auto* mu = new std::mutex();
  return *mu;
}

std::vector<InterceptRule>& RuleStore() {
  static auto* store = new std::vector<InterceptRule>();
  return *store;
}

int next_rule_id = 1;

// Thread-safe request observation log.
std::mutex& ObsMutex() {
  static auto* mu = new std::mutex();
  return *mu;
}

std::vector<ObservedRequest>& ObsStore() {
  static auto* store = new std::vector<ObservedRequest>();
  return *store;
}

// --- Request stream subscriber registry (thread-safe) ---
// Lives on the subscriber's delivery sequence; the throttle (any thread) posts
// frames to that sequence. A WeakPtr receiver makes cancel race-free: once the
// subscription (hence the sink) is destroyed, any already-posted frame no-ops.
class RequestSink {
 public:
  explicit RequestSink(RequestFrameCallback cb) : cb_(std::move(cb)) {}
  void Deliver(const ObservedRequest& r) { cb_.Run(r); }
  base::WeakPtr<RequestSink> GetWeak() { return weak_factory_.GetWeakPtr(); }

 private:
  RequestFrameCallback cb_;
  base::WeakPtrFactory<RequestSink> weak_factory_{this};
};

struct SubEntry {
  int id;
  scoped_refptr<base::SequencedTaskRunner> runner;
  base::WeakPtr<RequestSink> sink;
};

std::mutex& SubMutex() {
  static auto* mu = new std::mutex();
  return *mu;
}

std::vector<SubEntry>& SubStore() {
  static auto* store = new std::vector<SubEntry>();
  return *store;
}

int next_sub_id = 1;

// Fan a freshly observed request out to every active subscriber, each on its
// own delivery sequence. Safe to call from any thread (e.g. the throttle).
void BroadcastRequest(const ObservedRequest& obs) {
  std::lock_guard<std::mutex> lock(SubMutex());
  for (const auto& entry : SubStore()) {
    entry.runner->PostTask(
        FROM_HERE, base::BindOnce(&RequestSink::Deliver, entry.sink, obs));
  }
}

class RequestSubscriptionImpl : public RequestSubscription {
 public:
  explicit RequestSubscriptionImpl(RequestFrameCallback cb)
      : sink_(std::make_unique<RequestSink>(std::move(cb))) {
    std::lock_guard<std::mutex> lock(SubMutex());
    id_ = next_sub_id++;
    SubStore().push_back(
        {id_, base::SequencedTaskRunner::GetCurrentDefault(),
         sink_->GetWeak()});
  }

  ~RequestSubscriptionImpl() override {
    std::lock_guard<std::mutex> lock(SubMutex());
    auto& store = SubStore();
    for (auto it = store.begin(); it != store.end(); ++it) {
      if (it->id == id_) {
        store.erase(it);
        break;
      }
    }
    // sink_ is destroyed after the lock releases; its WeakPtrs invalidate, so
    // any frame already posted to the delivery sequence no-ops.
  }

 private:
  int id_ = 0;
  std::unique_ptr<RequestSink> sink_;
};

// A synthetic URLLoader spliced into the live load by InterceptResponse to
// replace the response BODY in place. It implements both halves of the
// loader<->client contract: it is the URLLoader the destination (renderer)
// now talks to, and the URLLoaderClient that receives the original network
// loader's residual events. The original body is drained and discarded; the
// destination reads the mock bytes from a fresh data pipe instead.
//
// Modeled on extensions/renderer/extension_localization_throttle.cc's
// ExtensionLocalizationURLLoader (the canonical in-tree consumer of
// URLLoaderThrottle::Delegate::InterceptResponse).
class BodyMockURLLoader : public network::mojom::URLLoaderClient,
                          public network::mojom::URLLoader,
                          public mojo::DataPipeDrainer::Client {
 public:
  BodyMockURLLoader(
      std::string mock_body,
      mojo::PendingRemote<network::mojom::URLLoaderClient> destination_client)
      : mock_body_(std::move(mock_body)),
        destination_client_(std::move(destination_client)) {}
  ~BodyMockURLLoader() override = default;

  void Start(
      mojo::PendingRemote<network::mojom::URLLoader> source_loader,
      mojo::PendingReceiver<network::mojom::URLLoaderClient>
          source_client_receiver,
      mojo::ScopedDataPipeConsumerHandle original_body,
      mojo::ScopedDataPipeProducerHandle producer_handle) {
    source_loader_.Bind(std::move(source_loader));
    source_client_receiver_.Bind(std::move(source_client_receiver));

    // Drain (and discard) the original network body so the source loader can
    // complete cleanly without a connection-reset on a dropped consumer.
    if (original_body) {
      drainer_ =
          std::make_unique<mojo::DataPipeDrainer>(this, std::move(original_body));
    }

    // Write the mock bytes into the pipe the destination now reads from.
    auto producer =
        std::make_unique<mojo::DataPipeProducer>(std::move(producer_handle));
    auto data = std::make_unique<std::string>(mock_body_);
    auto source = std::make_unique<mojo::StringDataSource>(
        *data, mojo::StringDataSource::AsyncWritingMode::
                   STRING_STAYS_VALID_UNTIL_COMPLETION);
    mojo::DataPipeProducer* producer_ptr = producer.get();
    producer_ptr->Write(
        std::move(source),
        base::BindOnce(
            [](std::unique_ptr<mojo::DataPipeProducer> producer,
               std::unique_ptr<std::string> data,
               base::OnceCallback<void(MojoResult)> on_written,
               MojoResult result) { std::move(on_written).Run(result); },
            std::move(producer), std::move(data),
            base::BindOnce(&BodyMockURLLoader::OnBodyWritten,
                           weak_factory_.GetWeakPtr())));
  }

  // network::mojom::URLLoaderClient (residual events from the network loader;
  // the response head already passed through, so a fresh response/redirect is
  // never expected here).
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr) override {}
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr,
                         mojo::ScopedDataPipeConsumerHandle,
                         std::optional<mojo_base::BigBuffer>) override {}
  void OnReceiveRedirect(const net::RedirectInfo&,
                         network::mojom::URLResponseHeadPtr) override {}
  void OnUploadProgress(int64_t,
                        int64_t,
                        OnUploadProgressCallback ack) override {
    std::move(ack).Run();
  }
  void OnTransferSizeUpdated(int32_t) override {}
  void OnComplete(const network::URLLoaderCompletionStatus&) override {
    // The original completion is irrelevant — the body is fully synthetic.
  }

  // network::mojom::URLLoader (control from the destination).
  void FollowRedirect(const std::vector<std::string>&,
                      const net::HttpRequestHeaders&,
                      const net::HttpRequestHeaders&,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    if (source_loader_)
      source_loader_->SetPriority(priority, intra_priority_value);
  }

  // mojo::DataPipeDrainer::Client — discard the original body.
  void OnDataAvailable(base::span<const uint8_t>) override {}
  void OnDataComplete() override { drainer_.reset(); }

 private:
  void OnBodyWritten(MojoResult result) {
    network::URLLoaderCompletionStatus status(
        result == MOJO_RESULT_OK ? net::OK : net::ERR_INSUFFICIENT_RESOURCES);
    if (result == MOJO_RESULT_OK) {
      status.decoded_body_length = static_cast<int64_t>(mock_body_.size());
      status.encoded_body_length = static_cast<int64_t>(mock_body_.size());
      status.encoded_data_length = static_cast<int64_t>(mock_body_.size());
    }
    destination_client_->OnComplete(status);
  }

  const std::string mock_body_;
  std::unique_ptr<mojo::DataPipeDrainer> drainer_;
  mojo::Receiver<network::mojom::URLLoaderClient> source_client_receiver_{this};
  mojo::Remote<network::mojom::URLLoader> source_loader_;
  mojo::Remote<network::mojom::URLLoaderClient> destination_client_;
  base::WeakPtrFactory<BodyMockURLLoader> weak_factory_{this};
};

// The Aurelian URLLoaderThrottle — applies intercept rules + observes.
class AurelianURLThrottle : public blink::URLLoaderThrottle {
 public:
  AurelianURLThrottle() = default;
  ~AurelianURLThrottle() override = default;

  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override {
    std::string url = request->url.spec();

    // Log the request for observation, then stream it to subscribers.
    ObservedRequest obs;
    obs.url = url;
    obs.method = request->method;
    {
      std::lock_guard<std::mutex> lock(ObsMutex());
      ObsStore().push_back(obs);
    }
    BroadcastRequest(obs);

    std::lock_guard<std::mutex> lock(RuleMutex());
    for (const auto& rule : RuleStore()) {
      if (url.find(rule.url_pattern) == std::string::npos)
        continue;

      if (rule.action == "block") {
        delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT);
        return;
      }
      if (rule.action == "redirect" && !rule.redirect_url.empty()) {
        request->url = GURL(rule.redirect_url);
        return;
      }
      // A "mock" rule replaces the response BODY in place — handled at
      // WillProcessResponse (the request must reach the response stage so the
      // throttle can splice a synthetic loader via InterceptResponse).
    }
    // Default pass-through — no rules matched.
  }

  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* response_head,
                           bool* defer) override {
    // Find a body-mock rule matching this response URL.
    InterceptRule matched;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(RuleMutex());
      for (const auto& rule : RuleStore()) {
        if (rule.action == "mock" && !rule.mock_body.empty() &&
            response_url.spec().find(rule.url_pattern) != std::string::npos) {
          matched = rule;
          found = true;
          break;
        }
      }
    }
    if (!found)
      return;

    // Rewrite the response head so the destination parses the mock correctly:
    // its declared type and length must match the bytes we are about to serve.
    response_head->mime_type = matched.mock_content_type;
    response_head->content_length =
        static_cast<int64_t>(matched.mock_body.size());
    if (response_head->headers) {
      response_head->headers->SetHeader("Content-Type",
                                        matched.mock_content_type);
      response_head->headers->RemoveHeader("Content-Length");
      response_head->headers->SetHeader(
          "Content-Length", base::NumberToString(matched.mock_body.size()));
    }

    // Fresh pipe: producer end we write the mock into; consumer end the
    // destination reads from (InterceptResponse swaps it into place).
    mojo::ScopedDataPipeConsumerHandle body;
    mojo::ScopedDataPipeProducerHandle producer_handle;
    if (mojo::CreateDataPipe(/*options=*/nullptr, producer_handle, body) !=
        MOJO_RESULT_OK) {
      return;  // Pass through unmodified on resource exhaustion.
    }

    mojo::PendingRemote<network::mojom::URLLoader> new_loader;
    mojo::PendingRemote<network::mojom::URLLoaderClient> destination_client;
    mojo::PendingReceiver<network::mojom::URLLoaderClient>
        destination_client_receiver =
            destination_client.InitWithNewPipeAndPassReceiver();
    mojo::PendingRemote<network::mojom::URLLoader> source_loader;
    mojo::PendingReceiver<network::mojom::URLLoaderClient> source_client_receiver;

    auto loader = std::make_unique<BodyMockURLLoader>(
        matched.mock_body, std::move(destination_client));
    BodyMockURLLoader* loader_raw = loader.get();
    // The loader lives as long as `new_loader` is connected (i.e. until the
    // ThrottlingURLLoader that now holds it is torn down).
    mojo::MakeSelfOwnedReceiver(std::move(loader),
                                new_loader.InitWithNewPipeAndPassReceiver());

    delegate_->InterceptResponse(std::move(new_loader),
                                 std::move(destination_client_receiver),
                                 &source_loader, &source_client_receiver, &body);
    // After the swap, `body` holds the ORIGINAL response body consumer.
    loader_raw->Start(std::move(source_loader),
                      std::move(source_client_receiver), std::move(body),
                      std::move(producer_handle));
  }
};

}  // namespace

// --- Rule API ---

int AddInterceptRule(InterceptRule rule) {
  std::lock_guard<std::mutex> lock(RuleMutex());
  rule.id = next_rule_id++;
  RuleStore().push_back(std::move(rule));
  return RuleStore().back().id;
}

bool RemoveInterceptRule(int rule_id) {
  std::lock_guard<std::mutex> lock(RuleMutex());
  auto& store = RuleStore();
  for (auto it = store.begin(); it != store.end(); ++it) {
    if (it->id == rule_id) {
      store.erase(it);
      return true;
    }
  }
  return false;
}

std::vector<InterceptRule> GetInterceptRules() {
  std::lock_guard<std::mutex> lock(RuleMutex());
  return RuleStore();
}

void ClearInterceptRules() {
  std::lock_guard<std::mutex> lock(RuleMutex());
  RuleStore().clear();
}

std::unique_ptr<blink::URLLoaderThrottle> CreateAurelianThrottle() {
  return std::make_unique<AurelianURLThrottle>();
}

// --- Request observation API ---

std::vector<ObservedRequest> GetObservedRequests() {
  std::lock_guard<std::mutex> lock(ObsMutex());
  return ObsStore();
}

void ClearObservedRequests() {
  std::lock_guard<std::mutex> lock(ObsMutex());
  ObsStore().clear();
}

std::unique_ptr<RequestSubscription> SubscribeRequests(
    RequestFrameCallback cb) {
  return std::make_unique<RequestSubscriptionImpl>(std::move(cb));
}

// --- Cookie API (UI thread, synchronous via RunLoop) ---

CookieResult GetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  CookieResult result;
  base::RunLoop loop;
  cookie_mgr->GetCookieList(
      GURL(url), net::CookieOptions::MakeAllInclusive(),
      net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](CookieResult* out, const std::string& want_name,
             base::RunLoop* rl,
             const net::CookieAccessResultList& cookies,
             const net::CookieAccessResultList&) {
            for (const auto& cookie_with_access : cookies) {
              if (std::string(cookie_with_access.cookie.Name()) == want_name) {
                out->ok = true;
                out->value = cookie_with_access.cookie.Value();
                break;
              }
            }
            if (!out->ok) out->error = "not-found";
            rl->Quit();
          },
          &result, name, &loop));
  loop.Run();
  return result;
}

std::vector<CookieInfo> GetAllCookies(content::BrowserContext* ctx,
                                      const std::string& url) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  std::vector<CookieInfo> result;
  base::RunLoop loop;
  cookie_mgr->GetCookieList(
      GURL(url), net::CookieOptions::MakeAllInclusive(),
      net::CookiePartitionKeyCollection(),
      base::BindOnce(
          [](std::vector<CookieInfo>* out, base::RunLoop* rl,
             const net::CookieAccessResultList& cookies,
             const net::CookieAccessResultList&) {
            for (const auto& cookie_with_access : cookies) {
              out->push_back({std::string(cookie_with_access.cookie.Name()),
                              std::string(cookie_with_access.cookie.Value())});
            }
            rl->Quit();
          },
          &result, &loop));
  loop.Run();
  return result;
}

CookieResult SetCookie(content::BrowserContext* ctx,
                       const std::string& url,
                       const std::string& name,
                       const std::string& value) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  GURL cookie_url(url);
  auto canonical = net::CanonicalCookie::CreateSanitizedCookie(
      cookie_url, name, value, std::string(cookie_url.host()), "/",
      base::Time(), base::Time(), base::Time(),
      cookie_url.SchemeIsCryptographic(),
      false,  // httponly
      net::CookieSameSite::LAX_MODE,
      net::COOKIE_PRIORITY_DEFAULT,
      std::nullopt,  // partition key
      /*status=*/nullptr);

  CookieResult result;
  if (!canonical) {
    result.error = "invalid-cookie";
    return result;
  }

  base::RunLoop loop;
  cookie_mgr->SetCanonicalCookie(
      *canonical, cookie_url,
      net::CookieOptions::MakeAllInclusive(),
      base::BindOnce(
          [](CookieResult* out, base::RunLoop* rl,
             net::CookieAccessResult access_result) {
            out->ok = access_result.status.IsInclude();
            if (!out->ok) out->error = "set-failed";
            rl->Quit();
          },
          &result, &loop));
  loop.Run();
  return result;
}

CookieResult DeleteCookie(content::BrowserContext* ctx,
                          const std::string& url,
                          const std::string& name) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  auto filter = network::mojom::CookieDeletionFilter::New();
  filter->url = GURL(url);
  filter->cookie_name = name;

  CookieResult result;
  base::RunLoop loop;
  cookie_mgr->DeleteCookies(
      std::move(filter),
      base::BindOnce(
          [](CookieResult* out, base::RunLoop* rl, uint32_t count) {
            out->ok = count > 0;
            if (!out->ok) out->error = "not-found";
            rl->Quit();
          },
          &result, &loop));
  loop.Run();
  return result;
}

int DeleteAllCookies(content::BrowserContext* ctx, const std::string& url) {
  auto* partition = ctx->GetDefaultStoragePartition();
  auto* cookie_mgr = partition->GetCookieManagerForBrowserProcess();

  // No cookie_name filter => delete every cookie applicable to the URL.
  auto filter = network::mojom::CookieDeletionFilter::New();
  filter->url = GURL(url);

  int deleted = 0;
  base::RunLoop loop;
  cookie_mgr->DeleteCookies(
      std::move(filter),
      base::BindOnce(
          [](int* out, base::RunLoop* rl, uint32_t count) {
            *out = static_cast<int>(count);
            rl->Quit();
          },
          &deleted, &loop));
  loop.Run();
  return deleted;
}

// --- Download API (UI thread) ---

void StartDownload(content::BrowserContext* ctx, const std::string& url) {
  auto* dm = ctx->GetDownloadManager();
  auto params = std::make_unique<download::DownloadUrlParameters>(
      GURL(url), TRAFFIC_ANNOTATION_FOR_TESTS);
  dm->DownloadUrl(std::move(params));
}

bool CancelDownload(content::BrowserContext* ctx, uint32_t id) {
  auto* dm = ctx->GetDownloadManager();
  download::DownloadItem* item = dm ? dm->GetDownload(id) : nullptr;
  if (!item) {
    return false;
  }
  item->Cancel(/*user_cancel=*/true);
  return true;
}

std::vector<DownloadInfo> GetDownloads(content::BrowserContext* ctx) {
  auto* dm = ctx->GetDownloadManager();
  content::DownloadManager::DownloadVector items;
  dm->GetAllDownloads(&items);

  std::vector<DownloadInfo> result;
  result.reserve(items.size());
  for (const auto& item : items) {
    DownloadInfo info;
    info.id = item->GetId();
    info.url = item->GetURL().spec();
    switch (item->GetState()) {
      case download::DownloadItem::IN_PROGRESS:
        info.state = "in_progress";
        break;
      case download::DownloadItem::COMPLETE:
        info.state = "complete";
        break;
      case download::DownloadItem::CANCELLED:
        info.state = "cancelled";
        break;
      case download::DownloadItem::INTERRUPTED:
        info.state = "interrupted";
        break;
      default:
        info.state = "unknown";
        break;
    }
    result.push_back(std::move(info));
  }
  return result;
}

}  // namespace aurelian
