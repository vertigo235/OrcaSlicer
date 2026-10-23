#ifndef slic3r_Utils_PrusaConnect_hpp_
#define slic3r_Utils_PrusaConnect_hpp_

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

struct PrusaConnectTool
{
    int         id = 0;
    bool        has_material = false;
    std::string material_id;
    std::string material_name;
    std::string material_type;
    std::string brand;
    std::string color;
};

struct PrusaConnectPrinter
{
    std::string                   id;
    std::string                   name;
    std::string                   organization_id;
    std::vector<PrusaConnectTool> tools;
};

struct PrusaConnectAmsSlot
{
    int         slot = 0;
    bool        has_material = false;
    std::string material_id;
    std::string material_name;
    std::string material_type;
    std::string brand;
    std::string color;
};

std::vector<PrusaConnectAmsSlot> map_prusa_connect_tools_to_ams(const PrusaConnectPrinter& printer);

struct PrusaConnectParseResult
{
    bool                    success = false;
    std::string             error;
    PrusaConnectPrinter     printer;
};

struct PrusaConnectResponse
{
    bool        ok = false;
    int         status = 0;
    std::string body;
    std::string error;
};

PrusaConnectResponse parse_prusa_connect_response(const std::string& response);

// Connect returns colors as RRGGBBAA strings. Orca's filament settings use
// #RRGGBB; alpha is intentionally ignored for filament appearance.
std::string normalize_prusa_connect_color(const std::string& value);

PrusaConnectParseResult parse_prusa_connect_graphql(const std::string& response);

// Parse the authenticated /app/printers response used by the Connect web UI.
// The response shape is private and has changed over time, so this deliberately
// accepts the common list wrappers and organization-id spellings.
std::vector<PrusaConnectPrinter> parse_prusa_connect_printers(const std::string& response,
                                                               std::string& error);

const char* prusa_connect_fff_graphql_query();

} // namespace Slic3r

#endif // slic3r_Utils_PrusaConnect_hpp_
