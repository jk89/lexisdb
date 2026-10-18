/*
 * Copyright 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#pragma once

#include "cql3/statements/cf_prop_defs.hh"
#include "cql3/column_identifier.hh"
#include "data_dictionary/data_dictionary.hh"
#include "exceptions/exceptions.hh"

namespace cql3 {

namespace statements {

/**
 * Class for common statement properties.
 */
class cf_properties final {
    const ::shared_ptr<cf_prop_defs> _properties = ::make_shared<cf_prop_defs>();
    bool _use_compact_storage = false;
    std::vector<std::pair<::shared_ptr<column_identifier>, bool>> _defined_ordering; // Insertion ordering is important
    // PARTITION ORDER BY: the direction of each listed partition key column.
    std::vector<std::pair<::shared_ptr<column_identifier>, bool>> _partition_ordering;

    static data_type with_direction(data_type type, bool is_reversed) {
        if (!is_reversed && type->is_reversed()) {
            return static_pointer_cast<const reversed_type_impl>(type)->underlying_type();
        }
        if (is_reversed && !type->is_reversed()) {
            return reversed_type_impl::get_instance(type);
        }
        return type;
    }
public:
    auto& properties() const {
        return _properties;
    }

    bool use_compact_storage() const {
        return _use_compact_storage;
    }

    void set_compact_storage() {
        _use_compact_storage = true;
    }

    auto& defined_ordering() const {
        return _defined_ordering;
    }

    data_type get_reversable_type(const column_identifier& t, data_type type) const {
        return with_direction(std::move(type), find_ordering_info(t).value_or(false));
    }

    auto& partition_ordering() const {
        return _partition_ordering;
    }

    // The type of partition key column t: reversed if PARTITION ORDER BY
    // declares it DESC, ascending otherwise.
    data_type get_partition_key_type(const column_identifier& t, data_type type) const {
        bool is_reversed = false;
        for (auto& p : _partition_ordering) {
            if (*p.first == t) {
                is_reversed = p.second;
            }
        }
        return with_direction(std::move(type), is_reversed);
    }

    std::optional<bool> find_ordering_info(const column_identifier& type) const {
        for (auto& t: _defined_ordering) {
            if (*(t.first) == type) {
                return t.second;
            }
        }
        return {};
    }

    void set_ordering(::shared_ptr<column_identifier> alias, bool reversed) {
        _defined_ordering.emplace_back(alias, reversed);
    }

    void set_partition_ordering(::shared_ptr<column_identifier> alias, bool reversed) {
        _partition_ordering.emplace_back(alias, reversed);
    }

    // Every column in PARTITION ORDER BY must be a partition key column,
    // listed once.
    template <typename IsPartitionKey>
    void validate_partition_ordering(IsPartitionKey&& is_partition_key) const {
        for (size_t i = 0; i < _partition_ordering.size(); ++i) {
            auto& id = *_partition_ordering[i].first;
            if (!is_partition_key(id)) {
                throw exceptions::invalid_request_exception(format("PARTITION ORDER BY lists {} which is not a partition key column", id));
            }
            for (size_t j = 0; j < i; ++j) {
                if (*_partition_ordering[j].first == id) {
                    throw exceptions::invalid_request_exception(format("PARTITION ORDER BY lists {} more than once", id));
                }
            }
        }
    }

    void validate(const data_dictionary::database db, sstring ks_name, const schema::extensions_map& schema_extensions) const {
        _properties->validate(db, std::move(ks_name), schema_extensions);
    }
};

}

}
