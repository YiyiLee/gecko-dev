/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this file,
* You can obtain one at http://mozilla.org/MPL/2.0/.
*/

[NavigatorProperty="push",
 JSImplementation="@mozilla.org/push/PushManager;1",
 CheckAnyPermissions="push",
 Pref="services.push.enabled"]
interface SimplePushManager {
    DOMRequest register();
    DOMRequest unregister(DOMString pushEndpoint);
    DOMRequest registrations();
};
