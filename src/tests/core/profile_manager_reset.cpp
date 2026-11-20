// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/hle/service/acc/profile_manager.h"
#include "tests/common/restore_eden_path.h"

namespace {

using Common::FS::EdenPath;
using Service::Account::ProfileManager;

// ResetUserSaveFile read the users back closed; the eden-cli resets once the config names the
// NAND, so a game booted from it found no open user. A reset now opens the current user, and
// makes the default one on an empty NAND, as the constructor does.
struct NandFixture {
    NandFixture() {
        std::random_device random;
        do {
            nand = std::filesystem::temp_directory_path() /
                   ("lichii-profile-reset-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(nand));
        Common::FS::SetEdenPath(EdenPath::NANDDir, nand);
    }
    ~NandFixture() {
        Tests::RestoreEdenPath(EdenPath::NANDDir, previous_nand);
        Settings::values.current_user = previous_user;
        std::error_code error;
        std::filesystem::remove_all(nand, error);
    }

    const std::filesystem::path previous_nand = Common::FS::GetEdenPath(EdenPath::NANDDir);
    const s32 previous_user = Settings::values.current_user.GetValue();
    std::filesystem::path nand;
};

TEST_CASE("A profile reset on an empty NAND makes and opens the default user", "[core][acc]") {
    NandFixture fixture;
    Settings::values.current_user = 0;
    ProfileManager manager;
    // The configured NAND holds no profiles yet.
    std::error_code error;
    std::filesystem::remove_all(fixture.nand / "system", error);

    manager.ResetUserSaveFile();
    CHECK(manager.GetUserCount() == 1);
    CHECK(manager.GetOpenUserCount() == 1);
}

TEST_CASE("A profile reset opens the current user", "[core][acc]") {
    NandFixture fixture;
    Settings::values.current_user = 0;
    ProfileManager manager;
    REQUIRE(manager.CreateNewUser(Common::UUID::MakeRandom(), "Second").IsSuccess());
    manager.WriteUserSaveFile();
    REQUIRE(manager.GetUserCount() == 2);

    Settings::values.current_user = 1;
    manager.ResetUserSaveFile();
    REQUIRE(manager.GetOpenUserCount() == 1);
    const auto second = manager.GetUser(1);
    REQUIRE(second.has_value());
    CHECK(manager.GetOpenUsers()[0] == *second);
}

TEST_CASE("A profile reset says whether it read profiles.dat", "[core][acc]") {
    NandFixture fixture;
    Settings::values.current_user = 0;
    ProfileManager manager;
    std::error_code error;
    std::filesystem::remove_all(fixture.nand / "system", error);
    manager.ResetUserSaveFile();
    CHECK_FALSE(manager.UserSaveFileRead());

    manager.WriteUserSaveFile();
    manager.ResetUserSaveFile();
    CHECK(manager.UserSaveFileRead());
}

} // namespace
