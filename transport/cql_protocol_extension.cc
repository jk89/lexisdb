/*
 * Copyright 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <seastar/core/print.hh>
#include "transport/cql_protocol_extension.hh"
#include "cql3/result_set.hh"
#include "exceptions/exceptions.hh"

#include <map>

namespace cql_transport {

static const std::map<cql_protocol_extension, seastar::sstring> EXTENSION_NAMES = {
    {cql_protocol_extension::LWT_ADD_METADATA_MARK, "SCYLLA_LWT_ADD_METADATA_MARK"},
    {cql_protocol_extension::RATE_LIMIT_ERROR, "SCYLLA_RATE_LIMIT_ERROR"},
    // Lexis sends ordered (blob) tokens, so it advertises its own key: drivers
    // that only understand upstream's bigint tokens do not negotiate it.
    {cql_protocol_extension::TABLETS_ROUTING_V1, "LEXIS_TABLETS_ROUTING_V1"},
    // Lexis: PREPARED responses carry a routing plan (lexis-prefix-routing-v1)
    // from which drivers compute the token a partition key prefix or range
    // read starts at.
    {cql_protocol_extension::PREFIX_ROUTING_V1, "LEXIS_PREFIX_ROUTING_V1"}
};

cql_protocol_extension_enum_set supported_cql_protocol_extensions() {
    return cql_protocol_extension_enum_set::full();
} 

const seastar::sstring& protocol_extension_name(cql_protocol_extension ext) {
    return EXTENSION_NAMES.at(ext);
}

std::vector<seastar::sstring> additional_options_for_proto_ext(cql_protocol_extension ext) {
    switch (ext) {
        case cql_protocol_extension::LWT_ADD_METADATA_MARK:
            return {format("LWT_OPTIMIZATION_META_BIT_MASK={:d}", cql3::prepared_metadata::LWT_FLAG_MASK)};
        case cql_protocol_extension::RATE_LIMIT_ERROR:
            return {format("ERROR_CODE={}", exceptions::exception_code::RATE_LIMIT_ERROR)};
        default:
            return {};
    }
}

} // namespace cql_transport
