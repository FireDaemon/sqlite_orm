#pragma once

#include <tuple>  //  std::tuple
#include <string>  //  std::string
#include <memory>  //  std::unique_ptr
#include <type_traits>  //  std::true_type, std::false_type, std::is_same, std::enable_if, std::is_member_pointer, std::is_member_function_pointer

#include "cxx_polyfill.h"
#include "type_traits.h"
#include "type_is_nullable.h"
#include "tuple_helper/tuple_helper.h"
#include "default_value_extractor.h"
#include "constraints.h"
#include "member_traits/member_traits.h"
#include "member_traits/field_member_traits.h"

namespace sqlite_orm {

    namespace internal {

        /**
         *  typename T::value_type if is_integral_constant_v<T>
         */
        template<typename T>
        using ice_value_type_t = std::enable_if_t<is_integral_constant_v<T>, value_type_t<T>>;

        struct basic_column {

            /**
             *  Column name. Specified during construction in `make_column`.
             */
            const std::string name;
        };

        /**
         *  This class stores single column info. column_t is a pair of [column_name:member_pointer] mapped to a storage
         *  O is a mapped class, e.g. User
         *  T is a mapped class'es field type, e.g. &User::name
         *  Op... is a constraints pack, e.g. primary_key_t, autoincrement_t etc
         */
        template<class O, class T, class G /* = const T& (O::*)() const*/, class S /* = void (O::*)(T)*/, class... Op>
        struct column_t : basic_column {
            using object_type = O;
            // the passed in type is either `integral_constant<F O::*member>` or the return type
            using _ice_or_field_type = T;
            // `F O::*` from `integral_constant<F O::*member>` or `T`
            using _member_pointer_or_field_type = polyfill::detected_or_t<T, ice_value_type_t, T>;
            // `F` from `F O::*` or `T`
            using field_type = polyfill::detected_or_t<_member_pointer_or_field_type,
                                                       field_type_t,
                                                       field_member_traits<_member_pointer_or_field_type>>;
            using constraints_type = std::tuple<Op...>;
            using member_pointer_t = field_type object_type::*;
            using getter_type = G;
            using setter_type = S;

            /**
             *  Member pointer used to read/write member
             */
            member_pointer_t member_pointer /* = nullptr*/;

            /**
             *  Getter member function pointer to get a value. If member_pointer is null than
             *  `getter` and `setter` must be not null
             */
            getter_type getter /* = nullptr*/;

            /**
             *  Setter member function
             */
            setter_type setter /* = nullptr*/;

            /**
             *  Constraints tuple
             */
            constraints_type constraints;

            column_t(std::string name_,
                     member_pointer_t member_pointer_,
                     getter_type getter_,
                     setter_type setter_,
                     constraints_type constraints_) :
                basic_column{move(name_)},
                member_pointer(member_pointer_), getter(getter_), setter(setter_), constraints(move(constraints_)) {}

            /**
             *  Simplified interface for `NOT NULL` constraint
             */
            bool not_null() const {
                return !type_is_nullable<field_type>::value;
            }

            template<class Opt>
            constexpr bool has() const {
                return tuple_helper::tuple_contains_type<Opt, constraints_type>::value;
            }

            /**
             *  Simplified interface for `DEFAULT` constraint
             *  @return string representation of default value if it exists otherwise nullptr
             */
            std::unique_ptr<std::string> default_value() const {
                std::unique_ptr<std::string> res;
                iterate_tuple(this->constraints, [&res](auto& v) {
                    auto dft = internal::default_value_extractor()(v);
                    if(dft) {
                        res = std::move(dft);
                    }
                });
                return res;
            }

            bool is_generated() const {
#if SQLITE_VERSION_NUMBER >= 3031000
                auto res = false;
                iterate_tuple(this->constraints, [&res](auto& constraint) {
                    using constraint_type = typename std::decay<decltype(constraint)>::type;
                    if(!res) {
                        res = is_generated_always<constraint_type>::value;
                    }
                });
                return res;
#else
                return false;
#endif
            }
        };

        // we are compelled to wrap all sfinae-implemented traits to prevent "error: type/value mismatch at argument 2 in template parameter list"
        namespace sfinae {
            /**
             *  Column with insertable primary key traits. Common case.
             */
            template<class T, class SFINAE = void>
            struct is_column_with_insertable_primary_key : public std::false_type {};

            /**
             *  Column with insertable primary key traits. Specialized case case.
             */
            template<class O, class T, class... Op>
            struct is_column_with_insertable_primary_key<
                column_t<O, T, Op...>,
                typename std::enable_if<(tuple_helper::tuple_contains_type<
                                         primary_key_t<>,
                                         typename column_t<O, T, Op...>::constraints_type>::value)>::type> {
                using column_type = column_t<O, T, Op...>;
                static constexpr bool value = is_primary_key_insertable<column_type>::value;
            };

            /**
             *  Column with noninsertable primary key traits. Common case.
             */
            template<class T, class SFINAE = void>
            struct is_column_with_noninsertable_primary_key : public std::false_type {};

            /**
             *  Column with noninsertable primary key traits. Specialized case case.
             */
            template<class O, class T, class... Op>
            struct is_column_with_noninsertable_primary_key<
                column_t<O, T, Op...>,
                typename std::enable_if<(tuple_helper::tuple_contains_type<
                                         primary_key_t<>,
                                         typename column_t<O, T, Op...>::constraints_type>::value)>::type> {
                using column_type = column_t<O, T, Op...>;
                static constexpr bool value = !is_primary_key_insertable<column_type>::value;
            };

        }

        /**
         *  Column traits. Common case.
         */
        template<class T>
        struct is_column : public std::false_type {};

        /**
         *  Column traits. Specialized case case.
         */
        template<class O, class T, class... Op>
        struct is_column<column_t<O, T, Op...>> : public std::true_type {};

        /**
         *  Column with insertable primary key traits.
         */
        template<class T>
        struct is_column_with_insertable_primary_key : public sfinae::is_column_with_insertable_primary_key<T> {};

        /**
         *  Column with noninsertable primary key traits.
         */
        template<class T>
        struct is_column_with_noninsertable_primary_key : public sfinae::is_column_with_noninsertable_primary_key<T> {};

        template<class T>
        struct column_field_type {
            using type = void;
        };

        template<class O, class T, class... Op>
        struct column_field_type<column_t<O, T, Op...>> {
            using type = typename column_t<O, T, Op...>::field_type;
        };

        template<class T, class SFINAE = void>
        struct column_field_expression {
            using type = void;
        };

        template<class O, class T, class... Op>
        struct column_field_expression<column_t<O, T, Op...>, match_if_not<is_integral_constant, T>> {
            using type = typename column_t<O, T, Op...>::member_pointer_t;
        };

        // match `F O::*member`
        template<class O, class F, F O::*m, class... Op>
        struct column_field_expression<column_t<O, ice_t<m>, Op...>, void> : ice_t<m> {};

        template<class T>
        struct column_constraints_type {
            using type = std::tuple<>;
        };

        template<class O, class T, class... Op>
        struct column_constraints_type<column_t<O, T, Op...>> {
            using type = typename column_t<O, T, Op...>::constraints_type;
        };

    }

    /**
     *  Column builder function. You should use it to create columns instead of constructor
     */
    template<class O,
             class T,
             typename = typename std::enable_if<!std::is_member_function_pointer<T O::*>::value>::type,
             class... Op>
    internal::column_t<O, T, const T& (O::*)() const, void (O::*)(T), Op...>
    make_column(const std::string& name, T O::*m, Op... constraints) {
        static_assert(internal::template constraints_size<Op...>::value == std::tuple_size<std::tuple<Op...>>::value,
                      "Incorrect constraints pack");
        static_assert(internal::is_field_member_pointer<T O::*>::value,
                      "second argument expected as a member field pointer, not member function pointer");
        return {name, m, nullptr, nullptr, std::make_tuple(constraints...)};
    }

    /**
     *  Column builder function. You should use it to create columns instead of constructor
     */
    template<class T,
             T m,
             class O = typename internal::field_member_traits<T>::object_type,
             class F = typename internal::field_member_traits<T>::field_type,
             class... Op>
    internal::column_t<O, std::integral_constant<T, m>, const F& (O::*)() const, void (O::*)(F), Op...>
    make_column(const std::string& name, std::integral_constant<T, m>, Op... constraints) {
        static_assert(internal::template constraints_size<Op...>::value == std::tuple_size<std::tuple<Op...>>::value,
                      "Incorrect constraints pack");
        static_assert(internal::is_field_member_pointer<T>::value,
                      "second argument expected as a member field pointer, not member function pointer");
        return {name, m, nullptr, nullptr, std::make_tuple(constraints...)};
    }

    /**
     *  Column builder function with setter and getter. You should use it to create columns instead of constructor
     */
    template<class G,
             class S,
             typename = typename std::enable_if<internal::is_getter<G>::value>::type,
             typename = typename std::enable_if<internal::is_setter<S>::value>::type,
             class... Op>
    internal::column_t<typename internal::setter_traits<S>::object_type,
                       typename internal::setter_traits<S>::field_type,
                       G,
                       S,
                       Op...>
    make_column(const std::string& name, S setter, G getter, Op... constraints) {
        static_assert(std::is_same<typename internal::setter_traits<S>::field_type,
                                   typename internal::getter_traits<G>::field_type>::value,
                      "Getter and setter must get and set same data type");
        static_assert(internal::template constraints_size<Op...>::value == std::tuple_size<std::tuple<Op...>>::value,
                      "Incorrect constraints pack");
        return {name, nullptr, getter, setter, std::make_tuple(constraints...)};
    }

    /**
     *  Column builder function with getter and setter (reverse order). You should use it to create columns instead of
     * constructor
     */
    template<class G,
             class S,
             typename = typename std::enable_if<internal::is_getter<G>::value>::type,
             typename = typename std::enable_if<internal::is_setter<S>::value>::type,
             class... Op>
    internal::column_t<typename internal::setter_traits<S>::object_type,
                       typename internal::setter_traits<S>::field_type,
                       G,
                       S,
                       Op...>
    make_column(const std::string& name, G getter, S setter, Op... constraints) {
        static_assert(std::is_same<typename internal::setter_traits<S>::field_type,
                                   typename internal::getter_traits<G>::field_type>::value,
                      "Getter and setter must get and set same data type");
        static_assert(internal::template constraints_size<Op...>::value == std::tuple_size<std::tuple<Op...>>::value,
                      "Incorrect constraints pack");
        return {name, nullptr, getter, setter, std::make_tuple(constraints...)};
    }

}
