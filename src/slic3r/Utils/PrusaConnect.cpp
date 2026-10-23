#include "PrusaConnect.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <initializer_list>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

using json = nlohmann::json;

std::string trim(std::string value)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string string_value(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    return {};
}

std::string first_string(const json& object, std::initializer_list<const char*> keys)
{
    if (!object.is_object())
        return {};
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end()) {
            const std::string result = trim(string_value(*it));
            if (!result.empty())
                return result;
        }
    }
    return {};
}

std::string nested_string(const json& object, const char* parent, std::initializer_list<const char*> keys)
{
    if (!object.is_object())
        return {};
    const auto it = object.find(parent);
    return it != object.end() ? first_string(*it, keys) : std::string();
}

const json* find_object(const json& object, std::initializer_list<const char*> keys)
{
    if (!object.is_object())
        return nullptr;
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_object())
            return &*it;
    }
    return nullptr;
}

bool numeric_id(const std::string& value, int& result)
{
    if (value.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed < 0 || parsed > INT_MAX)
        return false;
    result = static_cast<int>(parsed);
    return true;
}

bool is_credential_key(const std::string& key)
{
    std::string lowered;
    lowered.reserve(key.size());
    for (const unsigned char character : key)
        lowered.push_back(static_cast<char>(std::tolower(character)));
    return lowered.find("token") != std::string::npos || lowered.find("password") != std::string::npos ||
           lowered.find("secret") != std::string::npos || lowered.find("authorization") != std::string::npos ||
           lowered.find("cookie") != std::string::npos || lowered.find("api_key") != std::string::npos ||
           lowered.find("api-key") != std::string::npos || lowered.find("apikey") != std::string::npos;
}

bool contains_credential_key(const json& value)
{
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (is_credential_key(key) || contains_credential_key(child))
                return true;
        }
    } else if (value.is_array()) {
        for (const auto& child : value) {
            if (contains_credential_key(child))
                return true;
        }
    }
    return false;
}

bool response_contains_credentials(const json& envelope)
{
    if (contains_credential_key(envelope))
        return true;
    const auto body = envelope.find("body");
    if (body != envelope.end() && body->is_string()) {
        const json body_json = json::parse(body->get<std::string>(), nullptr, false);
        return !body_json.is_discarded() && contains_credential_key(body_json);
    }
    return false;
}

std::string graphql_error(const json& root)
{
    if (!root.is_object())
        return {};
    const auto it = root.find("errors");
    if (it == root.end() || !it->is_array() || it->empty())
        return {};

    std::string result;
    for (const auto& error : *it) {
        const std::string message = error.is_object() ? first_string(error, {"message", "error"}) : string_value(error);
        if (message.empty())
            continue;
        if (!result.empty())
            result += "; ";
        result += message;
    }
    return result.empty() ? "Connect GraphQL returned an error" : result;
}

const json* printer_node(const json& root)
{
    const auto data = root.find("data");
    if (data == root.end() || !data->is_object())
        return nullptr;
    const auto printer = data->find("printer");
    if (printer == data->end() || !printer->is_object())
        return nullptr;
    const auto node = printer->find("printer");
    return node != printer->end() && node->is_object() ? &*node : nullptr;
}

std::string organization_id(const json& object)
{
    if (!object.is_object())
        return {};
    const std::string explicit_id = first_string(object, {"organizationUuid", "organizationUUID", "organizationId", "organization_id", "org_id"});
    if (!explicit_id.empty())
        return explicit_id;
    if (const json* organization = find_object(object, {"organization"})) {
        const std::string id = first_string(*organization, {"uuid", "organizationId", "organization_id"});
        if (!id.empty())
            return id;
    }
    return {};
}

const json* printer_array(const json& root)
{
    if (root.is_array())
        return &root;
    if (!root.is_object())
        return nullptr;
    for (const char* key : {"printers", "items", "results"}) {
        const auto it = root.find(key);
        if (it != root.end()) {
            if (it->is_array())
                return &*it;
            if (it->is_object()) {
                if (const json* nested = printer_array(*it))
                    return nested;
            }
        }
    }
    if (const auto data = root.find("data"); data != root.end()) {
        if (data->is_array())
            return &*data;
        if (data->is_object())
            return printer_array(*data);
    }
    return nullptr;
}

} // namespace

PrusaConnectResponse parse_prusa_connect_response(const std::string& response)
{
    PrusaConnectResponse result;
    const json root = json::parse(response, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.error = "Connect bridge returned invalid JSON";
        return result;
    }
    if (response_contains_credentials(root)) {
        result.error = "Connect bridge returned credential-bearing data";
        return result;
    }
    const auto ok = root.find("ok");
    if (ok == root.end() || !ok->is_boolean()) {
        result.error = "Connect bridge returned an invalid success flag";
        return result;
    }
    result.ok = ok->get<bool>();
    if (const auto status = root.find("status"); status != root.end()) {
        if (!status->is_number_integer() && !status->is_number_unsigned()) {
            result.ok = false;
            result.error = "Connect bridge returned an invalid HTTP status";
            return result;
        }
        try {
            result.status = status->get<int>();
        } catch (const json::exception&) {
            result.ok = false;
            result.error = "Connect bridge returned an invalid HTTP status";
            return result;
        }
    }
    if (result.ok && result.status != 0 && (result.status < 200 || result.status >= 300))
        result.ok = false;
    if (const auto body = root.find("body"); body != root.end() && body->is_string())
        result.body = body->get<std::string>();
    if (const auto error = root.find("error"); error != root.end() && error->is_string())
        result.error = error->get<std::string>();
    if (!result.ok && result.error.empty()) {
        if (result.status != 0)
            result.error = "Connect request failed with HTTP status " + std::to_string(result.status);
        else
            result.error = "Connect request failed";
    }
    return result;
}

std::string normalize_prusa_connect_color(const std::string& raw_value)
{
    std::string value = trim(raw_value);
    if (value.empty())
        return {};
    if (value.size() > 1 && value[0] == '#')
        value.erase(value.begin());
    else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value.erase(0, 2);

    if (value.size() != 6 && value.size() != 8)
        return {};
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        return {};
    if (value.size() == 8)
        value.resize(6); // RRGGBBAA -> RRGGBB
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return "#" + value;
}

PrusaConnectParseResult parse_prusa_connect_graphql(const std::string& response)
{
    PrusaConnectParseResult result;
    const json root = json::parse(response, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.error = "Connect GraphQL returned invalid JSON";
        return result;
    }
    if (const std::string error = graphql_error(root); !error.empty()) {
        result.error = error;
        return result;
    }

    const json* node = printer_node(root);
    if (!node) {
        result.error = "Connect GraphQL returned no printer data";
        return result;
    }
    const std::string type = first_string(*node, {"__typename"});
    if (!type.empty() && type != "Printer") {
        result.error = first_string(*node, {"message", "error"});
        if (result.error.empty())
            result.error = "Connect did not return a printer";
        return result;
    }

    result.printer.id = first_string(*node, {"id", "uuid", "printerId", "printer_uuid"});
    result.printer.organization_id = organization_id(*node);
    const json* fff = find_object(*node, {"fff"});
    if (!fff) {
        result.error = "Connect printer has no FFF data";
        return result;
    }
    const auto tools = fff->find("tools");
    if (tools == fff->end() || !tools->is_array()) {
        result.error = "Connect printer returned no tool data";
        return result;
    }

    struct IndexedTool {
        PrusaConnectTool tool;
        bool             numeric = false;
    };
    std::vector<IndexedTool> indexed;
    indexed.reserve(tools->size());
    for (std::size_t index = 0; index < tools->size(); ++index) {
        const json& item = (*tools)[index];
        if (!item.is_object())
            continue;

        PrusaConnectTool tool;
        const std::string id = first_string(item, {"id", "toolId", "tool_id"});
        tool.id = static_cast<int>(index);
        bool numeric = false;
        if (!id.empty())
            numeric = numeric_id(id, tool.id);

        const json* package_instance = find_object(item, {"materialPackageInstance", "material_package_instance"});
        const json* material = package_instance ? find_object(*package_instance, {"package"}) : nullptr;
        material = material ? find_object(*material, {"material"}) : nullptr;
        if (material) {
            tool.has_material = true;
            tool.material_id = first_string(*material, {"uuid", "id"});
            tool.material_name = first_string(*material, {"name", "material"});
            tool.material_type = nested_string(*material, "type", {"abbreviation", "name", "id"});
            if (tool.material_type.empty())
                tool.material_type = first_string(item, {"materialType", "material_type", "type"});
            tool.brand = nested_string(*material, "brand", {"name", "id"});
            if (const json* color = find_object(*material, {"primaryColor", "primary_color"}))
                tool.color = normalize_prusa_connect_color(first_string(*color, {"colorRgba", "color_rgba", "value"}));
            if (tool.color.empty())
                tool.color = normalize_prusa_connect_color(first_string(*material, {"colorRgba", "color_rgba", "color", "colour"}));
        }
        indexed.push_back({std::move(tool), numeric});
    }

    // Numeric tool IDs define the physical order only when every ID is usable
    // and unique. Mixed or duplicate IDs are ambiguous, so retain Connect's
    // array order rather than associating a material with the wrong slot.
    bool sort_numeric = !indexed.empty();
    for (std::size_t i = 0; i < indexed.size() && sort_numeric; ++i) {
        if (!indexed[i].numeric) {
            sort_numeric = false;
            break;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (indexed[j].tool.id == indexed[i].tool.id) {
                sort_numeric = false;
                break;
            }
        }
    }
    if (sort_numeric) {
        std::stable_sort(indexed.begin(), indexed.end(), [](const IndexedTool& lhs, const IndexedTool& rhs) {
            return lhs.tool.id < rhs.tool.id;
        });
    }
    result.printer.tools.reserve(indexed.size());
    for (auto& entry : indexed)
        result.printer.tools.push_back(std::move(entry.tool));

    result.success = true;
    return result;
}

std::vector<PrusaConnectAmsSlot> map_prusa_connect_tools_to_ams(const PrusaConnectPrinter& printer)
{
    std::vector<PrusaConnectAmsSlot> result;
    result.reserve(printer.tools.size());
    for (std::size_t ordinal = 0; ordinal < printer.tools.size(); ++ordinal) {
        const PrusaConnectTool& tool = printer.tools[ordinal];
        result.push_back({static_cast<int>(ordinal), tool.has_material, tool.material_id, tool.material_name,
                          tool.material_type, tool.brand, tool.color});
    }
    return result;
}

std::vector<PrusaConnectPrinter> parse_prusa_connect_printers(const std::string& response, std::string& error)
{
    error.clear();
    const json root = json::parse(response, nullptr, false);
    if (root.is_discarded() || (!root.is_object() && !root.is_array())) {
        error = "Connect returned invalid printer JSON";
        return {};
    }
    if (const std::string message = graphql_error(root); !message.empty()) {
        error = message;
        return {};
    }
    const json* values = printer_array(root);
    if (!values) {
        error = "Connect returned no printer list";
        return {};
    }

    std::vector<PrusaConnectPrinter> result;
    for (const json& value : *values) {
        if (!value.is_object())
            continue;
        PrusaConnectPrinter printer;
        printer.id = first_string(value, {"uuid", "id", "printer_uuid", "printerId"});
        if (printer.id.empty())
            continue;
        printer.name = first_string(value, {"name", "printerName", "printer_name", "printer_type_name"});
        if (printer.name.empty())
            printer.name = printer.id;
        printer.organization_id = organization_id(value);
        result.push_back(std::move(printer));
    }
    if (result.empty())
        error = "Connect returned no usable printers";
    return result;
}

const char* prusa_connect_fff_graphql_query()
{
    return R"graphql(query GetPrinterFffTelemetry($organizationId: UUID!, $printerId: UUID!, $withMaterialDetails: Boolean = true) {
  printer {
    printer(organizationId: $organizationId, printerId: $printerId) {
      __typename
      ... on Printer {
        id
        printerType
        fff {
          hasMmuEnabled
          tools {
            id
            class
            type
            telemetry { active temp targetTemp }
            materialPackageInstance {
              __typename
              ... on FFFToolMaterialPackageInstance {
                uuid
                isFromNfc
                package {
                  material {
                    uuid
                    name
                    abbreviation
                    brand { name }
                    primaryColor { colorRgba }
                    ... @include(if: $withMaterialDetails) { type { abbreviation } }
                  }
                }
              }
              ... on ToolMaterialPackageInstanceEmpty { reason }
            }
          }
        }
      }
      ... on ConnectError { message }
      ... on DuplicateMaterialPackageInstanceError { message }
    }
  }
})graphql";
}

} // namespace Slic3r
