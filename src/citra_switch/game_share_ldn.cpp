// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Local wireless (LDN) for GameShare. Kept in its own translation unit because <switch.h>
// and the emulator's headers do not mix: libnx's Result / R_FAILED / Service clash with
// core/hle/result.h and the Service namespace.

#include <cstdio>
#include <cstring>
#include <switch.h>

#include "citra_switch/game_share_ldn.h"

namespace SwitchFrontend::GameShare {
namespace {

// Both ends must use the same values (see tools/gameshare/receiver-nx).
constexpr u16 LDN_SCENE_ID = 0x4453; // "DS"
constexpr u8 LDN_PASSPHRASE[0x10] = {0x44, 0x65, 0x6b, 0x6f, 0x70, 0x6f, 0x6e, 0x47,
                                     0x61, 0x6d, 0x65, 0x53, 0x68, 0x61, 0x72, 0x65};

void Append(char* log, std::size_t log_size, const char* format, unsigned value) {
    const std::size_t used = std::strlen(log);
    if (used + 1 < log_size) {
        std::snprintf(log + used, log_size - used, format, value);
    }
}

} // namespace

bool StartLocalWirelessHost(char* log, std::size_t log_size) {
    if (log_size > 0) {
        log[0] = '\0';
    }

    Result rc = ldnInitialize(LdnServiceType_User);
    Append(log, log_size, "ldnInitialize=0x%x ", rc);
    if (R_FAILED(rc)) {
        return false;
    }
    // Mixed Switch 1 / Switch 2 networks appear to need this protocol; harmless if refused.
    rc = ldnSetProtocol(LdnProtocol_Unknown3);
    Append(log, log_size, "ldnSetProtocol(NX+Ounce)=0x%x ", rc);

    rc = ldnOpenAccessPoint();
    Append(log, log_size, "ldnOpenAccessPoint=0x%x ", rc);
    if (R_FAILED(rc)) {
        ldnExit();
        return false;
    }

    LdnSecurityConfig security{};
    security.security_mode = LdnSecurityMode_Product;
    security.passphrase_size = sizeof(LDN_PASSPHRASE);
    std::memcpy(security.passphrase, LDN_PASSPHRASE, sizeof(LDN_PASSPHRASE));
    LdnUserConfig user{};
    std::strncpy(user.user_name, "Dekopon", sizeof(user.user_name) - 1);
    LdnNetworkConfig network{};
    // -1: the local communication id of the title the homebrew runs as. Both consoles have
    // to resolve to the same id, i.e. be launched through the same title.
    network.intent_id.local_communication_id = -1;
    network.intent_id.scene_id = LDN_SCENE_ID;
    network.channel = 0;
    network.node_count_max = 2;
    network.local_communication_version = 0;

    rc = ldnCreateNetwork(&security, &user, &network);
    Append(log, log_size, "ldnCreateNetwork=0x%x ", rc);
    if (R_FAILED(rc)) {
        ldnExit();
        return false;
    }

    LdnIpv4Address address{};
    LdnSubnetMask mask{};
    rc = ldnGetIpv4Address(&address, &mask);
    Append(log, log_size, "ldnGetIpv4Address=0x%x ", rc);
    Append(log, log_size, "address=0x%08x", address.addr);
    return true;
}

void StopLocalWirelessHost() {
    ldnExit();
}

void ReportUserActivity() {
    appletReportUserIsActive();
}

} // namespace SwitchFrontend::GameShare
