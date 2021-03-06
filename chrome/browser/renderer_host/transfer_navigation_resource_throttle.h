// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RENDERER_HOST_TRANSFER_NAVIGATION_RESOURCE_THROTTLE_H_
#define CHROME_BROWSER_RENDERER_HOST_TRANSFER_NAVIGATION_RESOURCE_THROTTLE_H_
#pragma once

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "content/public/browser/resource_throttle.h"

namespace net {
class URLRequest;
}

// This ResourceThrottle checks whether a navigation redirect will cause a
// renderer process swap. When that happens, we remember the request so
// that we can transfer it to be handled by the new renderer. This fixes
// http://crbug.com/79520
class TransferNavigationResourceThrottle : public content::ResourceThrottle {
 public:
  explicit TransferNavigationResourceThrottle(net::URLRequest* request);

  // content::ResourceThrottle implementation:
  virtual void WillRedirectRequest(const GURL& new_url, bool* defer) OVERRIDE;

 private:
  virtual ~TransferNavigationResourceThrottle();

  net::URLRequest* request_;

  DISALLOW_COPY_AND_ASSIGN(TransferNavigationResourceThrottle);
};


#endif  // CHROME_BROWSER_RENDERER_HOST_TRANSFER_NAVIGATION_RESOURCE_THROTTLE_H_
