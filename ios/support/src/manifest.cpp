/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/manifest.h"

#include "boxedvn/json.h"

namespace boxedvn {
namespace {

const json::Value* requireField(const json::Value& object, const std::string& key,
                                json::Type type, std::string& error) {
    const json::Value* field = object.find(key);
    if (field == nullptr) {
        error = "The manifest is missing the required field '" + key + "'.";
        return nullptr;
    }
    if (field->type != type) {
        error = "The manifest field '" + key + "' has the wrong type.";
        return nullptr;
    }
    return field;
}

std::string optionalString(const json::Value& object, const std::string& key) {
    const json::Value* field = object.find(key);
    if (field != nullptr && field->isString()) {
        return field->stringValue;
    }
    return std::string();
}

bool optionalBool(const json::Value& object, const std::string& key) {
    const json::Value* field = object.find(key);
    if (field != nullptr && field->isBool()) {
        return field->boolValue;
    }
    return false;
}

int64_t optionalInt(const json::Value& object, const std::string& key) {
    const json::Value* field = object.find(key);
    if (field != nullptr && field->isNumber()) {
        return static_cast<int64_t>(field->numberValue);
    }
    return 0;
}

std::vector<std::string> optionalStringArray(const json::Value& object,
                                             const std::string& key) {
    std::vector<std::string> values;
    const json::Value* field = object.find(key);
    if (field == nullptr || !field->isArray()) {
        return values;
    }
    for (const json::ValuePtr& element : field->arrayValue) {
        if (element && element->isString()) {
            values.push_back(element->stringValue);
        }
    }
    return values;
}

}  // namespace

std::string serialiseManifest(const GameManifest& manifest) {
    json::Writer writer;
    writer.beginObject();

    writer.key("schemaVersion");
    writer.value(static_cast<int64_t>(manifest.schemaVersion));

    writer.key("id");
    writer.value(manifest.id);

    writer.key("title");
    writer.value(manifest.title);

    writer.key("backend");
    writer.value(manifest.backend);

    writer.key("contentDirectory");
    writer.value(manifest.contentDirectory);

    writer.key("selectedExecutable");
    writer.value(manifest.selectedExecutable);

    writer.key("workingDirectory");
    writer.value(manifest.workingDirectory);

    writer.key("winePrefix");
    writer.value(manifest.winePrefix);

    writer.key("arguments");
    writer.beginArray();
    for (const std::string& argument : manifest.arguments) {
        writer.value(argument);
    }
    writer.endArray();

    writer.key("environment");
    writer.beginArray();
    for (const std::string& entry : manifest.environment) {
        writer.value(entry);
    }
    writer.endArray();

    writer.key("requestedWidth");
    writer.value(static_cast<int64_t>(manifest.requestedWidth));

    writer.key("requestedHeight");
    writer.value(static_cast<int64_t>(manifest.requestedHeight));

    writer.key("importedAtUnixSeconds");
    writer.value(manifest.importedAtUnixSeconds);

    writer.key("discoveredExecutables");
    writer.beginArray();
    for (const ManifestExecutable& executable : manifest.discoveredExecutables) {
        writer.beginObject();
        writer.key("relativePath");
        writer.value(executable.relativePath);
        writer.key("format");
        writer.value(executable.formatName);
        writer.key("architecture");
        writer.value(executable.architecture);
        writer.key("runnable");
        writer.value(executable.runnable);
        writer.key("subsystem");
        writer.value(static_cast<int64_t>(executable.subsystem));
        writer.key("diagnostic");
        writer.value(executable.diagnostic);
        writer.endObject();
    }
    writer.endArray();

    writer.endObject();

    std::string out = writer.str();
    out.push_back('\n');
    return out;
}

ManifestParseResult parseManifest(const std::string& text) {
    ManifestParseResult result;

    const json::ParseResult parsed = json::parse(text);
    if (!parsed.ok) {
        result.error = "The manifest is not valid JSON: " + parsed.error;
        return result;
    }
    if (!parsed.value || !parsed.value->isObject()) {
        result.error = "The manifest must be a JSON object.";
        return result;
    }
    const json::Value& root = *parsed.value;

    std::string error;
    const json::Value* schema =
        requireField(root, "schemaVersion", json::Type::Number, error);
    if (schema == nullptr) {
        result.error = error;
        return result;
    }
    const int schemaVersion = static_cast<int>(schema->numberValue);
    if (schemaVersion < 1) {
        result.error = "The manifest declares schemaVersion " +
                       std::to_string(schemaVersion) + ", which is not valid.";
        return result;
    }
    if (schemaVersion > kManifestSchemaVersion) {
        result.error = "The manifest declares schemaVersion " +
                       std::to_string(schemaVersion) +
                       ", but this build of BoxedVN understands at most " +
                       std::to_string(kManifestSchemaVersion) +
                       ". Update BoxedVN to open it.";
        return result;
    }

    const json::Value* id = requireField(root, "id", json::Type::String, error);
    if (id == nullptr) {
        result.error = error;
        return result;
    }
    if (id->stringValue.empty()) {
        result.error = "The manifest field 'id' is empty.";
        return result;
    }

    const json::Value* content =
        requireField(root, "contentDirectory", json::Type::String, error);
    if (content == nullptr) {
        result.error = error;
        return result;
    }

    GameManifest manifest;
    manifest.schemaVersion = schemaVersion;
    manifest.id = id->stringValue;
    manifest.title = optionalString(root, "title");
    manifest.backend = optionalString(root, "backend");
    if (manifest.backend.empty()) {
        manifest.backend = toString(RuntimeBackendID::BoxedwineX86);
    }
    manifest.contentDirectory = content->stringValue;
    manifest.selectedExecutable = optionalString(root, "selectedExecutable");
    manifest.workingDirectory = optionalString(root, "workingDirectory");
    manifest.winePrefix = optionalString(root, "winePrefix");
    manifest.arguments = optionalStringArray(root, "arguments");
    manifest.environment = optionalStringArray(root, "environment");
    manifest.requestedWidth =
        static_cast<uint32_t>(optionalInt(root, "requestedWidth"));
    manifest.requestedHeight =
        static_cast<uint32_t>(optionalInt(root, "requestedHeight"));
    manifest.importedAtUnixSeconds = optionalInt(root, "importedAtUnixSeconds");

    const json::Value* discovered = root.find("discoveredExecutables");
    if (discovered != nullptr && discovered->isArray()) {
        for (const json::ValuePtr& element : discovered->arrayValue) {
            if (!element || !element->isObject()) {
                continue;
            }
            ManifestExecutable executable;
            executable.relativePath = optionalString(*element, "relativePath");
            executable.formatName = optionalString(*element, "format");
            executable.architecture = optionalString(*element, "architecture");
            executable.runnable = optionalBool(*element, "runnable");
            executable.subsystem =
                static_cast<uint16_t>(optionalInt(*element, "subsystem"));
            executable.diagnostic = optionalString(*element, "diagnostic");
            if (!executable.relativePath.empty()) {
                manifest.discoveredExecutables.push_back(std::move(executable));
            }
        }
    }

    result.ok = true;
    result.manifest = std::move(manifest);
    return result;
}

}  // namespace boxedvn
