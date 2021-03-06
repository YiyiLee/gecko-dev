/* -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OfflineCacheUpdateParent.h"

#include "BackgroundUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/dom/TabParent.h"
#include "mozilla/ipc/URIUtils.h"
#include "mozilla/unused.h"
#include "nsOfflineCacheUpdate.h"
#include "nsIApplicationCache.h"
#include "nsIScriptSecurityManager.h"
#include "nsNetUtil.h"

using namespace mozilla::ipc;
using mozilla::BasePrincipal;
using mozilla::OriginAttributes;
using mozilla::dom::TabParent;

//
// To enable logging (see prlog.h for full details):
//
//    set NSPR_LOG_MODULES=nsOfflineCacheUpdate:5
//    set NSPR_LOG_FILE=offlineupdate.log
//
// this enables LogLevel::Debug level information and places all output in
// the file offlineupdate.log
//
extern PRLogModuleInfo *gOfflineCacheUpdateLog;

#undef LOG
#define LOG(args) MOZ_LOG(gOfflineCacheUpdateLog, mozilla::LogLevel::Debug, args)

#undef LOG_ENABLED
#define LOG_ENABLED() MOZ_LOG_TEST(gOfflineCacheUpdateLog, mozilla::LogLevel::Debug)

namespace mozilla {
namespace docshell {

//-----------------------------------------------------------------------------
// OfflineCacheUpdateParent::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ISUPPORTS(OfflineCacheUpdateParent,
                  nsIOfflineCacheUpdateObserver,
                  nsILoadContext)

//-----------------------------------------------------------------------------
// OfflineCacheUpdateParent <public>
//-----------------------------------------------------------------------------

// TODO: Bug 1191740 - Add OriginAttributes in TabContext
OfflineCacheUpdateParent::OfflineCacheUpdateParent(uint32_t aAppId,
                                                   bool aIsInBrowser)
    : mIPCClosed(false)
    , mOriginAttributes(aAppId, aIsInBrowser)
{
    // Make sure the service has been initialized
    nsOfflineCacheUpdateService::EnsureService();

    LOG(("OfflineCacheUpdateParent::OfflineCacheUpdateParent [%p]", this));
}

OfflineCacheUpdateParent::~OfflineCacheUpdateParent()
{
    LOG(("OfflineCacheUpdateParent::~OfflineCacheUpdateParent [%p]", this));
}

void
OfflineCacheUpdateParent::ActorDestroy(ActorDestroyReason why)
{
    mIPCClosed = true;
}

nsresult
OfflineCacheUpdateParent::Schedule(const URIParams& aManifestURI,
                                   const URIParams& aDocumentURI,
                                   const PrincipalInfo& aLoadingPrincipalInfo,
                                   const bool& stickDocument)
{
    LOG(("OfflineCacheUpdateParent::RecvSchedule [%p]", this));

    nsRefPtr<nsOfflineCacheUpdate> update;
    nsCOMPtr<nsIURI> manifestURI = DeserializeURI(aManifestURI);
    if (!manifestURI)
        return NS_ERROR_FAILURE;

    nsOfflineCacheUpdateService* service =
        nsOfflineCacheUpdateService::EnsureService();
    if (!service)
        return NS_ERROR_FAILURE;

    bool offlinePermissionAllowed = false;

    nsCOMPtr<nsIPrincipal> principal =
      BasePrincipal::CreateCodebasePrincipal(manifestURI, mOriginAttributes);

    nsresult rv = service->OfflineAppAllowed(
        principal, nullptr, &offlinePermissionAllowed);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!offlinePermissionAllowed)
        return NS_ERROR_DOM_SECURITY_ERR;

    nsCOMPtr<nsIURI> documentURI = DeserializeURI(aDocumentURI);
    if (!documentURI)
        return NS_ERROR_FAILURE;

    if (!NS_SecurityCompareURIs(manifestURI, documentURI, false))
        return NS_ERROR_DOM_SECURITY_ERR;

    // TODO: Bug 1197093 - add originAttributes to nsIOfflineCacheUpdate
    service->FindUpdate(manifestURI,
                        mOriginAttributes.mAppId,
                        mOriginAttributes.mInBrowser,
                        nullptr,
                        getter_AddRefs(update));
    if (!update) {
        update = new nsOfflineCacheUpdate();

        nsCOMPtr<nsIPrincipal> loadingPrincipal =
          PrincipalInfoToPrincipal(aLoadingPrincipalInfo, &rv);
        NS_ENSURE_SUCCESS(rv, rv);

        // Leave aDocument argument null. Only glues and children keep 
        // document instances.
        rv = update->Init(manifestURI, documentURI, loadingPrincipal, nullptr, nullptr,
                          mOriginAttributes.mAppId, mOriginAttributes.mInBrowser);
        NS_ENSURE_SUCCESS(rv, rv);

        rv = update->Schedule();
        NS_ENSURE_SUCCESS(rv, rv);
    }

    update->AddObserver(this, false);

    if (stickDocument) {
        nsCOMPtr<nsIURI> stickURI;
        documentURI->Clone(getter_AddRefs(stickURI));
        update->StickDocument(stickURI);
    }

    return NS_OK;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::UpdateStateChanged(nsIOfflineCacheUpdate *aUpdate, uint32_t state)
{
    if (mIPCClosed)
        return NS_ERROR_UNEXPECTED;

    LOG(("OfflineCacheUpdateParent::StateEvent [%p]", this));

    uint64_t byteProgress;
    aUpdate->GetByteProgress(&byteProgress);
    unused << SendNotifyStateEvent(state, byteProgress);

    if (state == nsIOfflineCacheUpdateObserver::STATE_FINISHED) {
        // Tell the child the particulars after the update has finished.
        // Sending the Finish event will release the child side of the protocol
        // and notify "offline-cache-update-completed" on the child process.
        bool isUpgrade;
        aUpdate->GetIsUpgrade(&isUpgrade);
        bool succeeded;
        aUpdate->GetSucceeded(&succeeded);

        unused << SendFinish(succeeded, isUpgrade);
    }

    return NS_OK;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::ApplicationCacheAvailable(nsIApplicationCache *aApplicationCache)
{
    if (mIPCClosed)
        return NS_ERROR_UNEXPECTED;

    NS_ENSURE_ARG(aApplicationCache);

    nsCString cacheClientId;
    aApplicationCache->GetClientID(cacheClientId);
    nsCString cacheGroupId;
    aApplicationCache->GetGroupID(cacheGroupId);

    unused << SendAssociateDocuments(cacheGroupId, cacheClientId);
    return NS_OK;
}

//-----------------------------------------------------------------------------
// OfflineCacheUpdateParent::nsILoadContext
//-----------------------------------------------------------------------------

NS_IMETHODIMP
OfflineCacheUpdateParent::GetAssociatedWindow(nsIDOMWindow * *aAssociatedWindow)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetTopWindow(nsIDOMWindow * *aTopWindow)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetTopFrameElement(nsIDOMElement** aElement)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetNestedFrameId(uint64_t* aId)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::IsAppOfType(uint32_t appType, bool *_retval)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetIsContent(bool *aIsContent)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetUsePrivateBrowsing(bool *aUsePrivateBrowsing)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}
NS_IMETHODIMP
OfflineCacheUpdateParent::SetUsePrivateBrowsing(bool aUsePrivateBrowsing)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::SetPrivateBrowsing(bool aUsePrivateBrowsing)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetUseRemoteTabs(bool *aUseRemoteTabs)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::SetRemoteTabs(bool aUseRemoteTabs)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetIsInBrowserElement(bool *aIsInBrowserElement)
{
    *aIsInBrowserElement = mOriginAttributes.mInBrowser;
    return NS_OK;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetAppId(uint32_t *aAppId)
{
    *aAppId = mOriginAttributes.mAppId;
    return NS_OK;
}

NS_IMETHODIMP
OfflineCacheUpdateParent::GetOriginAttributes(JS::MutableHandleValue aAttrs)
{
    JSContext* cx = nsContentUtils::GetCurrentJSContext();
    MOZ_ASSERT(cx);

    bool ok = ToJSValue(cx, mOriginAttributes, aAttrs);
    NS_ENSURE_TRUE(ok, NS_ERROR_FAILURE);
    return NS_OK;
}

} // namespace docshell
} // namespace mozilla
