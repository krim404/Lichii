// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/util/game.h"

#include <algorithm>
#include <fmt/format.h>
#include <QFile>
#include <QSaveFile>
#include "common/fs/fs.h"
#include "common/logging.h"

namespace QtCommon::Game {

std::filesystem::path CustomArtworkPath(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "artwork" /
           fmt::format("{:016X}.png", program_id);
}

std::vector<u8> ReadCustomArtwork(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    constexpr qint64 MaxArtworkBytes = 16 * 1024 * 1024;
    QFile file(QString::fromStdString(Common::FS::PathToUTF8String(CustomArtworkPath(program_id))));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxArtworkBytes) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    return std::vector<u8>(bytes.begin(), bytes.end());
}

bool SaveCustomArtwork(u64 program_id, const QImage& image) {
    if (program_id == 0 || image.isNull()) {
        return false;
    }
    constexpr int MaxSide = 1024;
    const QImage scaled =
        std::max(image.width(), image.height()) > MaxSide
            ? image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : image;
    const auto path = CustomArtworkPath(program_id);
    if (!Common::FS::CreateDirs(path.parent_path())) {
        LOG_ERROR(Frontend, "Cannot create the custom artwork folder");
        return false;
    }
    // Write to a temporary file and swap it in, so a failed save never leaves a broken image.
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    if (!file.open(QIODevice::WriteOnly) || !scaled.save(&file, "PNG") || !file.commit()) {
        LOG_ERROR(Frontend, "Cannot write custom artwork for {:016X}", program_id);
        return false;
    }
    return true;
}

// True when nothing is left at the path. A path that cannot be checked counts as still there.
static bool RemoveCustomFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const bool still_there = std::filesystem::exists(path, ec);
    return !still_there && !ec;
}

bool RemoveCustomArtwork(u64 program_id) {
    return RemoveCustomFile(CustomArtworkPath(program_id));
}

std::filesystem::path CustomTitlePath(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "names" /
           fmt::format("{:016X}.txt", program_id);
}

// Larger name files are ignored when read, so a longer name is never saved.
constexpr qint64 MaxCustomTitleBytes = 4096;

std::string ReadCustomTitle(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    QFile file(QString::fromStdString(Common::FS::PathToUTF8String(CustomTitlePath(program_id))));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxCustomTitleBytes) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).simplified().toStdString();
}

bool SaveCustomTitle(u64 program_id, const std::string& title) {
    if (program_id == 0) {
        return false;
    }
    const QString name = QString::fromStdString(title).simplified();
    const auto path = CustomTitlePath(program_id);
    if (name.isEmpty()) {
        return RemoveCustomFile(path);
    }
    const QByteArray bytes = name.toUtf8();
    if (bytes.size() > MaxCustomTitleBytes) {
        LOG_ERROR(Frontend, "The custom name for {:016X} is too long", program_id);
        return false;
    }
    if (!Common::FS::CreateDirs(path.parent_path())) {
        LOG_ERROR(Frontend, "Cannot create the custom name folder");
        return false;
    }
    QSaveFile file(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit()) {
        LOG_ERROR(Frontend, "Cannot write the custom name for {:016X}", program_id);
        return false;
    }
    return true;
}

} // namespace QtCommon::Game
