#ifndef LITEHTML_STYLESHEET_H
#define LITEHTML_STYLESHEET_H

#include <utility>

#include "css_selector.h"
#include "css_tokenizer.h"
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <algorithm>

namespace litehtml
{

    // https://www.w3.org/TR/cssom-1/#css-declarations
    struct raw_declaration
    {
        using vector = std::vector<raw_declaration>;

        // property name
        std::string name;
        // default value is specified here to get rid of gcc warning "missing initializer for member"
        css_token_vector value;

        bool important = false;

        operator bool() const
        {
            return name != "";
        }
    };

    // intermediate half-parsed rule that is used internally by the parser
    class raw_rule
    {
      public:
        using ptr    = std::shared_ptr<raw_rule>;
        using vector = std::vector<ptr>;

        enum rule_type
        {
            qualified,
            at
        };

        raw_rule(rule_type _type, std::string _name = {}) :
            type(_type),
            name(std::move(_name))
        {
        }

        rule_type type;
        // An at-rule has a name, a prelude consisting of a list of component values, and an optional
        // block consisting of a simple {} block.
        std::string name;
        // https://www.w3.org/TR/css-syntax-3/#qualified-rule
        // A qualified rule has a prelude consisting of a list of component values, and a block consisting
        // of a simple
        // {} block. Note: Most qualified rules will be style rules, where the prelude is a selector and
        // the block a list of declarations.
        css_token_vector prelude;
        css_token        block;
    };

    class css
    {
        css_selector::vector                               m_selectors;
        std::unordered_map<string_id, std::vector<size_t>> m_tag_candidates;
        std::unordered_map<string_id, std::vector<size_t>> m_id_candidates;
        std::unordered_map<string_id, std::vector<size_t>> m_class_candidates;
        std::vector<size_t>                                m_universal_candidates;
        bool                                               m_candidates_ready = false;

      public:
        // Each selector belongs to one mandatory rightmost id/class/tag bucket.
        // Merge by original cascade rank; full matching still decides admission.
        template <class Function>
        void for_each_candidate(string_id tag, string_id id, const std::vector<string_id>& classes,
                                Function process) const
        {
            if(!m_candidates_ready)
            {
                for(const auto& selector : m_selectors)
                {
                    process(selector);
                }
                return;
            }
            struct Cursor
            {
                const std::vector<size_t>* rows;
                size_t                     next = 0;
            };
            constexpr size_t                  InlineBuckets = 8;
            std::array<Cursor, InlineBuckets> local{};
            size_t                            count = 0;
            std::vector<Cursor>               spill;
            const auto                        add = [&](const std::vector<size_t>& rows) {
                if(rows.empty())
                {
                    return;
                }
                if(count < local.size() && spill.empty())
                {
                    local[count++] = {&rows};
                } else
                {
                    if(spill.empty())
                    {
                        spill.assign(local.begin(), local.begin() + count);
                    }
                    spill.push_back({&rows});
                }
            };
            const auto lookup = [&](const auto& buckets, string_id key) {
                const auto found = buckets.find(key);
                if(found != buckets.end())
                {
                    add(found->second);
                }
            };
            add(m_universal_candidates);
            lookup(m_tag_candidates, tag);
            lookup(m_id_candidates, id);
            std::unordered_set<string_id> seen;
            for(size_t index = 0; index < classes.size(); ++index)
            {
                const auto name = classes[index];
                if(classes.size() > InlineBuckets
                       ? !seen.insert(name).second
                       : std::find(classes.begin(), classes.begin() + index, name) != classes.begin() + index)
                {
                    continue;
                }
                lookup(m_class_candidates, name);
            }
            const auto merge = [&](auto begin, auto end) {
                const auto later = [](const Cursor& a, const Cursor& b) {
                    return (*a.rows)[a.next] > (*b.rows)[b.next];
                };
                std::make_heap(begin, end, later);
                while(begin != end)
                {
                    std::pop_heap(begin, end, later);
                    auto& cursor = *(end - 1);
                    process(m_selectors[(*cursor.rows)[cursor.next++]]);
                    if(cursor.next == cursor.rows->size())
                    {
                        --end;
                    } else
                    {
                        std::push_heap(begin, end, later);
                    }
                }
            };
            if(spill.empty())
            {
                merge(local.begin(), local.begin() + count);
            } else
            {
                merge(spill.begin(), spill.end());
            }
        }

        const css_selector::vector& selectors() const
        {
            return m_selectors;
        }

        template <class Input>
        void parse_css_stylesheet(const Input& input, const std::string& baseurl, const std::shared_ptr<document>& doc,
                                  const media_query_list_list::ptr& media = nullptr, bool top_level = true);

        void sort_selectors();

      private:
        bool parse_style_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                              const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void parse_import_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                               const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void add_selector(const css_selector::ptr& selector);
    };

    inline void css::add_selector(const css_selector::ptr& selector)
    {
        selector->m_order = static_cast<int>(m_selectors.size());
        m_selectors.push_back(selector);
        m_candidates_ready = false;
    }

} // namespace litehtml

#endif // LITEHTML_STYLESHEET_H
