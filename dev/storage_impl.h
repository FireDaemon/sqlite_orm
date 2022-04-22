#pragma once

#include <sqlite3.h>
#include <string>  //  std::string
#include <type_traits>  //  std::is_same, std::decay
#include <vector>  //  std::vector
#include <typeindex>  //  std::type_index
#include <utility>  //  std::forward, std::move

#include "type_traits.h"
#include "constraints.h"
#include "table_info.h"
#include "sync_schema_result.h"
#include "storage_lookup.h"

namespace sqlite_orm {

    namespace internal {

        struct storage_impl_base {

            bool table_exists(const std::string& tableName, sqlite3* db) const;

            void rename_table(sqlite3* db, const std::string& oldName, const std::string& newName) const;

            static bool calculate_remove_add_columns(std::vector<table_info*>& columnsToAdd,
                                                     std::vector<table_info>& storageTableInfo,
                                                     std::vector<table_info>& dbTableInfo);
        };

        /**
         *  This is a generic implementation. Used as a tail in storage_impl inheritance chain
         */
        template<class... Ts>
        struct storage_impl;

        template<class H, class... Ts>
        struct storage_impl<H, Ts...> : storage_impl<Ts...> {
            using super = storage_impl<Ts...>;
            using table_type = H;

            storage_impl(H h, Ts... ts) : super{std::forward<Ts>(ts)...}, table{std::move(h)} {}

            table_type table;

            template<class L>
            void for_each(const L& l) const {
                this->super::for_each(l);
                l(*this);
            }
        };

        template<>
        struct storage_impl<> : storage_impl_base {

            template<class L>
            void for_each(const L&) const {}
        };
    }
}

#include "static_magic.h"
#include "select_constraints.h"

// interface functions
namespace sqlite_orm {
    namespace internal {

        template<class S>
        using table_type_or_none_t = polyfill::detected_t<table_type_t, S>;

        template<class Lookup, class S, satisfies<is_storage_impl, S> = true>
        auto lookup_table(const S& strg) {
            const auto& tImpl = find_impl<Lookup>(strg);
            return static_if<std::is_same<decltype(tImpl), const storage_impl<>&>::value>(
                empty_callable<std::nullptr_t>(),
                [](const auto& tImpl) {
                    return &tImpl.table;
                })(tImpl);
        }

        template<class S, satisfies<is_storage_impl, S> = true>
        std::string find_table_name(const S& strg, const std::type_index& ti) {
            return static_if<std::is_same<S, storage_impl<>>::value>(
                empty_callable<std::string>(),
                [&ti](const auto& tImpl) {
                    using qualified_type = std::decay_t<decltype(tImpl)>;
                    std::type_index thisTypeIndex =
                        typeid(std::conditional_t<std::is_void<storage_label_of_t<qualified_type>>::value,
                                                  storage_object_type_t<qualified_type>,
                                                  storage_label_of_t<qualified_type>>);
                    return thisTypeIndex == ti ? tImpl.table.name
                                               : find_table_name<typename qualified_type::super>(tImpl, ti);
                })(strg);
        }

        template<class Lookup, class S, satisfies<is_storage_impl, S> = true>
        std::string lookup_table_name(const S& strg) {
            const auto& tImpl = find_impl<Lookup>(strg);
            return static_if<std::is_same<decltype(tImpl), const storage_impl<>&>{}>(empty_callable<std::string>(),
                                                                                     [](const auto& tImpl) {
                                                                                         return tImpl.table.name;
                                                                                     })(tImpl);
        }

        template<class Lookup, class S, satisfies<is_storage_impl, S> = true>
        const std::string& get_table_name(const S& strg) {
            return pick_impl<Lookup>(strg).table.name;
        }

        /**
         *  Find column name by its type and member pointer.
         */
        template<class O, class F, class S, satisfies<is_storage_impl, S> = true>
        const std::string* find_column_name(const S& strg, F O::*field) {
            return pick_impl<O>(strg).table.find_column_name(field);
        }

        /**
         *  Materialize column pointer:
         *  1. by explicit object type and member pointer.
         *  2. by label type and member pointer.
         */
        template<class O, class F, class S, satisfies<is_storage_impl, S> = true>
        constexpr decltype(auto) materialize_column_pointer(const S&, const column_pointer<O, F>& cp) {
            return cp.field;
        }

        /**
         *  Materialize column pointer:
         *  3. by label type and alias_holder<>.
         */
        template<class Label, class ColAlias, class S, satisfies<is_storage_impl, S> = true>
        constexpr decltype(auto) materialize_column_pointer(const S&,
                                                            const column_pointer<Label, alias_holder<ColAlias>>&) {
            using timpl_type = storage_pick_impl_t<S, Label>;
            using cte_mapper_type = storage_cte_mapper_type_t<timpl_type>;

            // filter all column references [`alias_holder<>`]
            using alias_types_tuple =
                transform_tuple_t<typename cte_mapper_type::final_colrefs_tuple, alias_holder_type_or_none>;

            // lookup index in alias_types_tuple by Alias
            constexpr auto ColIdx = tuple_index_of_v<ColAlias, alias_types_tuple>;
            static_assert(ColIdx != -1, "No such column mapped into the CTE.");

            return &aliased_field<ColAlias, std::tuple_element_t<ColIdx, cte_mapper_type::fields_type>>;
        }

        /**
         *  Find column name by:
         *  1. by explicit object type and member pointer.
         *  2. by label type and member pointer.
         */
        template<class O, class F, class S, satisfies<is_storage_impl, S> = true>
        const std::string* find_column_name(const S& strg, const column_pointer<O, F>& cp) {
            auto field = materialize_column_pointer(strg, cp);
            return pick_impl<O>(strg).table.find_column_name(field);
        }

        /**
         *  Find column name by:
         *  3. by label type and alias_holder<>.
         */
        template<class Label, class ColAlias, class S, satisfies<is_storage_impl, S> = true>
        constexpr decltype(auto) find_column_name(const S& s, const column_pointer<Label, alias_holder<ColAlias>>&) {
            using timpl_type = storage_pick_impl_t<S, Label>;
            using cte_mapper_type = storage_cte_mapper_type_t<timpl_type>;
            using elements_t = typename timpl_type::table_type::elements_type;
            using column_idxs = filter_tuple_sequence_t<elements_t, is_column>;

            // note: even though the columns contain the [`aliased_field<>::*`] we perform the lookup using the column references.
            // filter all column references [`alias_holder<>`]
            using alias_types_tuple =
                transform_tuple_t<typename cte_mapper_type::final_colrefs_tuple, alias_holder_type_or_none>;

            // lookup index of ColAlias in alias_types_tuple
            static constexpr auto I = tuple_index_of_v<ColAlias, alias_types_tuple>;
            static_assert(I != -1, "No such column mapped into the CTE.");

            // note: we could "materialize" the alias to an `aliased_field<>::*` and use the regular `table_t<>::find_column_name()` mechanism;
            //       however we have the column index already.
            // lookup column in table_t<>'s elements
            constexpr size_t ColIdx = index_sequence_value(I, column_idxs{});
            auto& timpl = pick_impl<Label>(s);
            return &std::get<ColIdx>(timpl.table.elements).name;
        }

        /**
         *  Find column name by:
         *  4. by label type and index constant.
         */
        template<class Label, size_t I, class S, satisfies<is_storage_impl, S> = true>
        constexpr decltype(auto) find_column_name(const S& s,
                                                  const column_pointer<Label, polyfill::index_constant<I>>&) {
            using timpl_type = storage_pick_impl_t<S, Label>;
            using column_idxs = filter_tuple_sequence_t<typename timpl_type::table_type::elements_type, is_column>;

            // lookup column in table_t<>'s elements
            constexpr size_t ColIdx = index_sequence_value(I, column_idxs{});
            static_assert(ColIdx < column_idxs{}.size(), "No such column mapped into the CTE.");

            auto& timpl = pick_impl<Label>(s);
            return &std::get<ColIdx>(timpl.table.elements).name;
        }
    }
}
