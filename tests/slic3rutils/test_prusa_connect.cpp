#include <catch2/catch_test_macros.hpp>

#include <string>

#include "slic3r/Utils/PrusaConnect.hpp"

using namespace Slic3r;

TEST_CASE("Prusa Connect response envelopes do not expose credentials", "[PrusaConnect]")
{
    const auto result = parse_prusa_connect_response(R"json({
        "command": "prusa_connect_result", "kind": "filaments", "ok": true,
        "status": 200, "body": "{\"data\":{}}"
    })json");
    REQUIRE(result.ok);
    REQUIRE(result.status == 200);
    REQUIRE(result.body == R"({"data":{}})");
    REQUIRE(result.error.empty());

    const auto credential_bearing = parse_prusa_connect_response(R"json({
        "ok": true, "status": 200,
        "body": "{\"data\":{\"access_token\":\"must-not-cross\"}}"
    })json");
    REQUIRE_FALSE(credential_bearing.ok);
    REQUIRE(credential_bearing.error == "Connect bridge returned credential-bearing data");

    const auto error = parse_prusa_connect_response(R"json({
        "ok": false, "status": 401, "error": "Connect session is not authenticated"
    })json");
    REQUIRE_FALSE(error.ok);
    REQUIRE(error.status == 401);
    REQUIRE(error.error == "Connect session is not authenticated");

    const auto inconsistent_status = parse_prusa_connect_response(R"json({
        "ok": true, "status": 401, "body": "{}"
    })json");
    REQUIRE_FALSE(inconsistent_status.ok);
    REQUIRE(inconsistent_status.error == "Connect request failed with HTTP status 401");

    const auto malformed = parse_prusa_connect_response(R"json({
        "ok": true, "status": "200", "body": "[]"
    })json");
    REQUIRE_FALSE(malformed.ok);
    REQUIRE(malformed.error == "Connect bridge returned an invalid HTTP status");
}

TEST_CASE("Prusa Connect GraphQL query requests material colors without credentials", "[PrusaConnect]")
{
    const std::string query = prusa_connect_fff_graphql_query();
    REQUIRE(query.find("tools") != std::string::npos);
    REQUIRE(query.find("materialPackageInstance") != std::string::npos);
    REQUIRE(query.find("primaryColor") != std::string::npos);
    REQUIRE(query.find("access_token") == std::string::npos);
    REQUIRE(query.find("refresh_token") == std::string::npos);
}

TEST_CASE("Prusa Connect colors normalize to Orca RGB", "[PrusaConnect]")
{
    REQUIRE(normalize_prusa_connect_color("F7F7DEFF") == "#F7F7DE");
    REQUIRE(normalize_prusa_connect_color("#00ff00") == "#00FF00");
    REQUIRE(normalize_prusa_connect_color("0xFFA50080") == "#FFA500");
    REQUIRE(normalize_prusa_connect_color("not-a-color").empty());
}

TEST_CASE("Prusa Connect GraphQL tools are ordered and preserve empty slots", "[PrusaConnect]")
{
    const std::string response = R"json({
      "data": { "printer": { "printer": {
        "__typename": "Printer",
        "id": "printer-1",
        "fff": { "tools": [
          { "id": "2", "materialPackageInstance": {
            "package": { "material": {
              "uuid": "mat-2", "name": "Green PLA", "brand": { "name": "Example" },
              "type": { "abbreviation": "PLA" }, "primaryColor": { "colorRgba": "00FF00FF" }
            }}
          }},
          { "id": "1", "materialPackageInstance": { "__typename": "ToolMaterialPackageInstanceEmpty" } },
          { "id": "0", "materialPackageInstance": {
            "package": { "material": {
              "uuid": "mat-0", "name": "Orange PETG", "type": { "abbreviation": "PETG" },
              "primaryColor": { "colorRgba": "FFA500FF" }
            }}
          }}
        ] }
      }}}
    })json";

    const auto result = parse_prusa_connect_graphql(response);
    REQUIRE(result.success);
    REQUIRE(result.error.empty());
    REQUIRE(result.printer.id == "printer-1");
    REQUIRE(result.printer.tools.size() == 3);
    REQUIRE(result.printer.tools[0].id == 0);
    REQUIRE(result.printer.tools[0].has_material);
    REQUIRE(result.printer.tools[0].material_type == "PETG");
    REQUIRE(result.printer.tools[0].color == "#FFA500");
    REQUIRE(result.printer.tools[1].id == 1);
    REQUIRE_FALSE(result.printer.tools[1].has_material);
    REQUIRE(result.printer.tools[2].id == 2);
    REQUIRE(result.printer.tools[2].material_id == "mat-2");

    const auto slots = map_prusa_connect_tools_to_ams(result.printer);
    REQUIRE(slots.size() == 3);
    REQUIRE(slots[0].slot == 0);
    REQUIRE(slots[0].has_material);
    REQUIRE(slots[0].material_type == "PETG");
    REQUIRE(slots[0].material_name == "Orange PETG");
    REQUIRE(slots[0].color == "#FFA500");
    REQUIRE(slots[1].slot == 1);
    REQUIRE_FALSE(slots[1].has_material);
    REQUIRE(slots[1].material_id.empty());
    REQUIRE(slots[1].material_type.empty());
    REQUIRE(slots[1].color.empty());
    REQUIRE(slots[2].material_name == "Green PLA");
    REQUIRE(slots[2].brand == "Example");
    REQUIRE(slots[2].color == "#00FF00");
}

TEST_CASE("Prusa Connect GraphQL errors are surfaced without response details", "[PrusaConnect]")
{
    const auto result = parse_prusa_connect_graphql(R"json({
        "errors": [{"message": "Not authorized"}],
        "data": null
    })json");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error == "Not authorized");
}

TEST_CASE("Prusa Connect accepts an empty GraphQL errors array", "[PrusaConnect]")
{
    const auto result = parse_prusa_connect_graphql(R"json({
      "errors": [], "data": { "printer": { "printer": {
        "__typename": "Printer", "fff": { "tools": [] }
      }}}
    })json");
    REQUIRE(result.success);
}

TEST_CASE("Prusa Connect preserves ambiguous tool order", "[PrusaConnect]")
{
    const auto result = parse_prusa_connect_graphql(R"json({
      "data": { "printer": { "printer": {
        "__typename": "Printer", "fff": { "tools": [
          {"id":"2"}, {"id":"renamed"}, {"id":"0"}
        ]}
      }}}
    })json");
    REQUIRE(result.success);
    REQUIRE(result.printer.tools.size() == 3);
    REQUIRE(result.printer.tools[0].id == 2);
    REQUIRE(result.printer.tools[1].id == 1);
    REQUIRE(result.printer.tools[2].id == 0);
    const auto slots = map_prusa_connect_tools_to_ams(result.printer);
    REQUIRE(slots[0].slot == 0);
    REQUIRE(slots[1].slot == 1);
    REQUIRE(slots[2].slot == 2);
}

TEST_CASE("Prusa Connect prefers explicit organization identifiers", "[PrusaConnect]")
{
    std::string error;
    const auto printers = parse_prusa_connect_printers(R"json({
      "printers": [{
        "uuid": "printer-1", "organization_id": "org-1", "team": {"id": 7}
      }]
    })json", error);
    REQUIRE(error.empty());
    REQUIRE(printers.size() == 1);
    REQUIRE(printers[0].organization_id == "org-1");
}

TEST_CASE("Prusa Connect printer list accepts common response wrappers", "[PrusaConnect]")
{
    std::string error;
    const auto printers = parse_prusa_connect_printers(R"json({
      "data": {"printers": {"items": [{
        "uuid": "printer-1", "name": "CORE One", "organizationId": "org-1"
      }]}}
    })json", error);
    REQUIRE(error.empty());
    REQUIRE(printers.size() == 1);
    REQUIRE(printers[0].id == "printer-1");
    REQUIRE(printers[0].organization_id == "org-1");
}
