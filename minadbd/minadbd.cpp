/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "minadbd.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <android-base/properties.h>

#include "adb.h"
#include "adb_auth.h"
#include "transport.h"

int minadbd_main() {
    adb_device_banner = "sideload";

    signal(SIGPIPE, SIG_IGN);

    // We can't require authentication for sideloading. http://b/22025550.
    auth_required = false;

    init_transport_registration();

    bool is_usb = false;
    if (access(USB_FFS_ADB_EP0, F_OK) == 0) {
        // Listen on USB.
        usb_init();
        is_usb = true;
    }

    // If one of these properties is set, also listen on that port.
    // If one of the properties isn't set and we couldn't listen on usb, listen
    // on the default port.
    std::string prop_port = android::base::GetProperty("service.adb.tcp.port", "");
    if (prop_port.empty()) {
        prop_port = android::base::GetProperty("persist.adb.tcp.port", "");
    }

    int port;
    if (sscanf(prop_port.c_str(), "%d", &port) == 1 && port > 0) {
        // Listen on TCP port specified by service.adb.tcp.port property.
        local_init(port);
    } else if (!is_usb) {
        // Listen on default port.
        local_init(DEFAULT_ADB_LOCAL_TRANSPORT_PORT);
    }

    //VLOG(ADB) << "Event loop starting";
    fdevent_loop();

    return 0;
}
