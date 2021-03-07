/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "CryptoType.h"

#include <string.h>

#include <android-base/logging.h>
#include <cutils/properties.h>


const CryptoType& lookup_crypto_algorithm(const CryptoType table[], int table_len,
                                          const CryptoType& default_alg, const char* property) {
    char paramstr[PROPERTY_VALUE_MAX];

    property_get(property, paramstr, default_alg.get_config_name());
    for (int i = 0; i < table_len; i++) {
        if (strcmp(paramstr, table[i].get_config_name()) == 0) {
            return table[i];
        }
    }
    LOG(ERROR) << "Invalid name (" << paramstr << ") for " << property << ".  Defaulting to "
               << default_alg.get_config_name() << ".";
    return default_alg;
}
