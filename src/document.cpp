#include "html.h"
#include "document.h"
#include "document_container.h"
#include "stylesheet.h"
#include "css_parser.h"
#include "html_tag.h"
#include "el_text.h"
#include "el_para.h"
#include "el_space.h"
#include "el_anchor.h"
#include "el_base.h"
#include "el_body.h"
#include "el_break.h"
#include "el_cdata.h"
#include "el_comment.h"
#include "el_div.h"
#include "el_font.h"
#include "el_image.h"
#include "el_link.h"
#include "el_para.h"
#include "el_script.h"
#include "el_space.h"
#include "el_style.h"
#include "el_table.h"
#include "el_td.h"
#include "el_text.h"
#include "el_title.h"
#include "el_tr.h"
#include "gumbo.h"
#include "html_tag.h"
#include "render_block.h"
#include "render_item.h"
#include "render_table.h"
#include "stylesheet.h"
#include "types.h"
#include <algorithm>
#include <cstring>

namespace litehtml
{

    namespace
    {
        // A prefix may only use declarations whose result is independent of omitted
        // siblings and document height. Parse real CSS tokens, including escapes and
        // comments, rather than searching raw style text for forbidden spellings.
        bool prefix_declarations(const css_token_vector& tokens, document_container* container)
        {
            raw_declaration::vector declarations;
            raw_rule::vector        nested;
            css_parser(tokens).consume_style_block_contents(declarations, nested);
            if(!nested.empty())
            {
                return false;
            }
            for(const auto& declaration : declarations)
            {
                container->check_cancelled();
                const auto& name    = declaration.name;
                const bool  spacing = name == "margin" || name == "margin-top" || name == "margin-right" ||
                                      name == "margin-bottom" || name == "margin-left" || name == "padding" ||
                                      name == "padding-top" || name == "padding-right" || name == "padding-bottom" ||
                                      name == "padding-left";
                const bool  text = name == "color" || name == "background-color" || name == "font" ||
                                   name == "font-family" || name == "font-size" || name == "font-style" ||
                                   name == "font-weight" || name == "font-variant" || name == "line-height" ||
                                   name == "letter-spacing" || name == "word-spacing" || name == "text-align" ||
                                   name == "text-decoration" || name == "text-decoration-line" ||
                                   name == "text-decoration-color" || name == "text-transform" || name == "white-space";
                if(!spacing && !text)
                {
                    return false;
                }
                // A shortened line box can let an omitted block's glyphs paint back
                // into the prefix. Normal font metrics keep ink local to each block.
                if(name == "line-height")
                {
                    auto value = declaration.value;
                    remove_whitespace(value);
                    if(value.size() != 1 || value.front().type != IDENT || value.front().ident() != "normal")
                    {
                        return false;
                    }
                }
                if(name == "font" && std::any_of(declaration.value.begin(), declaration.value.end(),
                                                 [](const css_token& token) { return token.ch == '/'; }))
                {
                    return false;
                }
                if(spacing)
                {
                    for(const auto& token : declaration.value)
                    {
                        if(token.is_component_value())
                        {
                            return false;
                        }
                        if((token.type == NUMBER || token.type == DIMENSION || token.type == PERCENTAGE) &&
                           token.n.number < 0)
                        {
                            return false;
                        }
                    }
                }
            }
            return true;
        }
        bool prefix_stylesheet(const string& source, document_container* container)
        {
            for(const auto& rule : css_parser::parse_stylesheet(source, true))
            {
                container->check_cancelled();
                if(rule->type != raw_rule::qualified || !prefix_declarations(rule->block.value, container))
                {
                    return false;
                }
                for(const auto& tokens : parse_comma_separated_list(rule->prelude))
                {
                    auto selector = make_shared<css_selector>();
                    if(!selector->parse(get_repr(tokens), no_quirks_mode))
                    {
                        continue;
                    }
                    for(; selector; selector = selector->m_left)
                    {
                        for(const auto& attribute : selector->m_right.m_attrs)
                        {
                            if(attribute.type == select_pseudo_element)
                            {
                                return false;
                            }
                            if(attribute.type != select_pseudo_class)
                            {
                                continue;
                            }
                            if(!attribute.selector_list.empty())
                            {
                                return false;
                            }
                            switch(attribute.name)
                            {
                            case _root_:
                            case _first_child_:
                            case _first_of_type_:
                            case _nth_child_:
                            case _nth_of_type_:
                                break;
                            default:
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }
    } // namespace

    document::document(document_container* container)
    {
        m_container = container;
    }

    document::~document()
    {
        m_over_element = m_active_element = nullptr;
        if(m_container)
        {
            for(auto& font : m_fonts)
            {
                m_container->delete_font(font.second.font);
            }
        }
    }

    document::ptr document::createFromString(estring str, document_container* container, const string& master_styles,
                                             const string&                                    user_styles,
                                             const std::function<bool(const document::ptr&)>& first_document,
                                             size_t initial_blocks, size_t maximum_nodes, size_t maximum_depth,
                                             bool reuse_computed_styles, std::string_view validated_utf8,
                                             std::string_view initial_text, int initial_occurrence)
    {
        // Create litehtml::document
        document::ptr doc        = make_shared<document>(container);
        doc->m_user_style_text   = user_styles;
        doc->m_master_style_text = master_styles;
        doc->m_maximum_nodes     = maximum_nodes;
        doc->m_maximum_depth     = maximum_depth;

        // Parse document into GumboOutput
        const auto release_output = [](GumboOutput* value) { gumbo_destroy_output(&kGumboDefaultOptions, value); };
        // Borrow validated UTF-8 only during this call. Gumbo's original-tag views
        // expire after from_gumbo has copied the final elements into owning nodes.
        const auto parse = [&] {
            if(validated_utf8.empty())
            {
                return doc->parse_html(std::move(str));
            }
            auto options          = kGumboDefaultOptions;
            options.userdata      = container;
            options.maximum_nodes = maximum_nodes ? maximum_nodes + 1 : 0;
            options.maximum_depth = maximum_depth ? maximum_depth + 1 : 0;
            options.cancelled = [](void* context) { return static_cast<document_container*>(context)->is_cancelled(); };
            return gumbo_parse_with_options(&options, validated_utf8.data(), validated_utf8.size());
        };
        std::unique_ptr<GumboOutput, decltype(release_output)> parsed(parse(), release_output);
        container->check_cancelled();
        GumboOutput* output = parsed.get();

        // Inspect the complete parsed tree before exposing any prefix. The fast
        // path admits independent flow with prefix-stable author CSS; dependency on
        // omitted siblings, tables, counters or resources requires full creation.
        if(output->limit_exceeded || !output->root)
        {
            return {};
        }
        GumboNode* body        = nullptr;
        bool       independent = prefix_stylesheet(user_styles, container);
        struct PendingNode
        {
            GumboNode* node;
            size_t     depth;
        };
        std::vector<PendingNode> pending{
            {output->root, 0}
        };
        size_t     nodes       = 0;
        GumboNode* anchor_node = nullptr;
        int        occurrence  = 0;
        while(first_document && independent && !pending.empty())
        {
            container->check_cancelled();
            const auto current = pending.back();
            auto*      node    = current.node;
            pending.pop_back();
            const auto count_node = [&](const char* text) {
                ++nodes;
                if(!initial_text.empty() && initial_text == text && occurrence++ == initial_occurrence)
                {
                    anchor_node = node;
                }
            };
            if(node->type == GUMBO_NODE_TEXT)
            {
                container->split_text(node->v.text.text, count_node, count_node);
            } else if(node->type == GUMBO_NODE_WHITESPACE)
            {
                nodes += strlen(node->v.text.text);
            } else
            {
                ++nodes;
            }
            if((maximum_nodes && nodes > maximum_nodes) || (maximum_depth && current.depth > maximum_depth))
            {
                independent = false;
                break;
            }
            if(node->type != GUMBO_NODE_ELEMENT)
            {
                continue;
            }
            const auto& element = node->v.element;
            switch(element.tag)
            {
            case GUMBO_TAG_HTML:
            case GUMBO_TAG_HEAD:
            case GUMBO_TAG_TITLE:
            case GUMBO_TAG_META:
            case GUMBO_TAG_STYLE:
            case GUMBO_TAG_BODY:
            case GUMBO_TAG_P:
            case GUMBO_TAG_DIV:
            case GUMBO_TAG_H1:
            case GUMBO_TAG_H2:
            case GUMBO_TAG_H3:
            case GUMBO_TAG_H4:
            case GUMBO_TAG_H5:
            case GUMBO_TAG_H6:
            case GUMBO_TAG_BLOCKQUOTE:
            case GUMBO_TAG_PRE:
            case GUMBO_TAG_SPAN:
            case GUMBO_TAG_B:
            case GUMBO_TAG_STRONG:
            case GUMBO_TAG_I:
            case GUMBO_TAG_EM:
            case GUMBO_TAG_U:
            case GUMBO_TAG_S:
            case GUMBO_TAG_CODE:
            case GUMBO_TAG_BR:
                break;
            default:
                independent = false;
                break;
            }
            if(element.tag == GUMBO_TAG_STYLE)
            {
                if(!node->parent || node->parent->type != GUMBO_NODE_ELEMENT ||
                   node->parent->v.element.tag != GUMBO_TAG_HEAD)
                {
                    independent = false;
                }
                string source;
                for(unsigned int i = 0; i < element.children.length; ++i)
                {
                    auto* child = static_cast<GumboNode*>(element.children.data[i]);
                    if(child->type == GUMBO_NODE_TEXT || child->type == GUMBO_NODE_WHITESPACE)
                    {
                        source += child->v.text.text;
                    }
                }
                independent = independent && prefix_stylesheet(source, container);
            }
            if(element.tag == GUMBO_TAG_BODY)
            {
                body = node;
            }
            for(unsigned int i = 0; i < element.attributes.length; ++i)
            {
                auto*        attribute = static_cast<GumboAttribute*>(element.attributes.data[i]);
                const string name(attribute->name);
                if(name == "style")
                {
                    independent = independent && prefix_declarations(normalize(string(attribute->value)), container);
                } else if(name != "id" && name != "class" && name != "title" && name != "lang" && name != "dir" &&
                          element.tag != GUMBO_TAG_META)
                {
                    independent = false;
                }
            }
            for(unsigned int i = element.children.length; i > 0; --i)
            {
                pending.push_back({static_cast<GumboNode*>(element.children.data[i - 1]), current.depth + 1});
            }
        }
        if(first_document && independent && body && initial_blocks > 0)
        {
            // Include the exact source occurrence before constructing any prefix.
            // A middle-anchor request must not rebuild every doubling prefix.
            if(!initial_text.empty())
            {
                auto* branch = anchor_node;
                while(branch && branch->parent != body)
                {
                    branch = branch->parent;
                }
                if(branch)
                {
                    initial_blocks += branch->index_within_parent + 1;
                } else
                {
                    initial_blocks = body->v.element.children.length;
                }
            }
            auto&      children = body->v.element.children;
            const auto total    = children.length;
            struct RestoreLength
            {
                GumboVector& children;
                unsigned int total;
                ~RestoreLength()
                {
                    children.length = total;
                }
            } restore{children, total};
            // Promote the exact first-screen DOM rather than creating the chapter
            // again. Prefix-stable selectors cannot change existing cascades when
            // later flow siblings arrive. Only new subtrees need declaration and
            // computed-style work; rendering is rebuilt once after tail creation.
            size_t count            = std::min(initial_blocks, size_t(total));
            children.length         = static_cast<unsigned int>(count);
            auto       prefix       = from_gumbo(output, container, master_styles, user_styles, reuse_computed_styles,
                                                 maximum_nodes, maximum_depth);
            auto       body_element = prefix->m_root ? prefix->m_root->select_one("body") : nullptr;
            const auto extend       = [&](size_t end) {
                children.length               = total;
                prefix->m_maximum_nodes       = maximum_nodes;
                constexpr size_t source_batch = 32;
                elements_list    added;
                auto             last_existing = body_element->children().end();
                const bool       had_children  = !body_element->children().empty();
                if(had_children)
                {
                    --last_existing;
                }
                while(count < end)
                {
                    container->check_cancelled();
                    const auto batch_end = std::min(end, count + source_batch);
                    added.clear();
                    for(; count < batch_end; ++count)
                    {
                        prefix->create_node(children.data[count], added, true, true);
                    }
                    for(const auto& child : added)
                    {
                        body_element->appendChild(child);
                    }
                }
                // Attach the complete tail before matching forward sibling selectors.
                // Otherwise every source batch would invalidate and rebuild all
                // sibling ordinals, making nth-child preparation quadratic.
                {
                    selector_scope matching(*prefix);
                    auto           child = had_children ? std::next(last_existing) : body_element->children().begin();
                    for(; child != body_element->children().end(); ++child)
                    {
                        container->check_cancelled();
                        (*child)->apply_stylesheet(prefix->m_master_css);
                        (*child)->parse_attributes();
                        (*child)->apply_stylesheet(prefix->m_styles);
                        (*child)->apply_stylesheet(prefix->m_user_css);
                        (*child)->compute_styles();
                    }
                }
                prefix->m_maximum_nodes = 0;
                prefix->m_root_render.reset();
                prefix->m_tabular_elements.clear();
                prefix->m_root_render = prefix->m_root->create_render_item(nullptr);
                prefix->fix_tables_layout();
                if(prefix->m_root_render)
                {
                    prefix->m_root_render = prefix->m_root_render->init();
                }
            };
            if(body_element)
            {
                while(count < total)
                {
                    container->check_cancelled();
                    if(first_document(prefix))
                    {
                        extend(total);
                        return prefix;
                    }
                    const auto next =
                        std::min(size_t(total), initial_text.empty() ? count * 2 : count + initial_blocks / 16 + 8);
                    extend(next);
                }
                return prefix;
            }
        }
        container->check_cancelled();
        return from_gumbo(output, container, master_styles, user_styles, reuse_computed_styles, maximum_nodes,
                          maximum_depth);
    }

    document::ptr document::from_gumbo(GumboOutput* output, document_container* container, const string& master_styles,
                                       const string& user_styles, bool reuse_computed_styles, size_t maximum_nodes,
                                       size_t maximum_depth)
    {
        auto doc                     = make_shared<document>(container);
        doc->m_reuse_computed_styles = reuse_computed_styles;
        doc->m_maximum_nodes         = maximum_nodes;
        doc->m_maximum_depth         = maximum_depth;
        doc->m_user_style_text       = user_styles;
        doc->m_master_style_text     = master_styles;
        // mode must be set before doc->create_node because it is used in html_tag::set_attr
        switch(output->document->v.document.doc_type_quirks_mode)
        {
        case GUMBO_DOCTYPE_NO_QUIRKS:
            doc->m_mode = no_quirks_mode;
            break;
        case GUMBO_DOCTYPE_QUIRKS:
            doc->m_mode = quirks_mode;
            break;
        case GUMBO_DOCTYPE_LIMITED_QUIRKS:
            doc->m_mode = limited_quirks_mode;
            break;
        }

        // Create litehtml::elements.
        elements_list root_elements;
        doc->create_node(output->root, root_elements, true, true);
        element::ptr root;
        if(!root_elements.empty())
        {
            root = root_elements.back();
        }
        doc->m_maximum_nodes = 0;

        container->check_cancelled();

        doc->finalize_from_external_root(root, master_styles, user_styles);

        return doc;
    }

    void document::set_document_mode(document_mode mode)
    {
        m_mode = mode;
    }

    void document::finalize_from_external_root(const std::shared_ptr<element>& root, const std::string& master_styles,
                                               const std::string& user_styles)
    {
        // Finalization accumulates state that is never rolled back: the parsed stylesheets, the media
        // lists, the tabular elements, the render tree, and the used selectors of every element. A
        // second run would duplicate all of it, so a document is finalized once, exactly like a parsed
        // one. Build a new document to rebuild.
        if(m_finalized)
        {
            return;
        }
        m_finalized         = true;
        m_master_style_text = master_styles;
        m_user_style_text   = user_styles;
        selector_scope matching(*this);

        m_root = root;

        if(master_styles != "")
        {
            m_master_css.parse_css_stylesheet(master_styles, "", shared_from_this());
            m_master_css.sort_selectors();
        }
        if(user_styles != "")
        {
            m_user_css.parse_css_stylesheet(user_styles, "", shared_from_this());
            m_user_css.sort_selectors();
        }

        // Let's process created elements tree
        if(m_root)
        {
            container()->get_media_features(m_media);

            m_root->set_pseudo_class(_root_, true);

            // apply master CSS
            m_root->apply_stylesheet(m_master_css);

            // parse elements attributes
            m_root->parse_attributes();

            // parse style sheets linked in document
            for(const auto& css : m_css)
            {
                media_query_list_list::ptr media;
                if(css.media != "")
                {
                    auto mq_list = parse_media_query_list(css.media, shared_from_this());
                    media        = std::make_shared<media_query_list_list>();
                    media->add(mq_list);
                }
                m_styles.parse_css_stylesheet(css.text, css.baseurl, shared_from_this(), media);
            }
            // Sort css selectors using CSS rules.
            m_styles.sort_selectors();

            // Apply media features.
            update_media_lists(m_media);

            // Apply parsed styles.
            m_root->apply_stylesheet(m_styles);

            // Apply user styles if any
            m_root->apply_stylesheet(m_user_css);

            // Initialize element::m_css
            m_root->compute_styles();

            rebuild_render_tree();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#change-the-encoding
    encoding adjust_meta_encoding(encoding meta_encoding, encoding current_encoding)
    {
        // 1.
        if(current_encoding == encoding::utf_16le || current_encoding == encoding::utf_16be)
        {
            return current_encoding;
        }

        // 2.
        if(meta_encoding == encoding::utf_16le || meta_encoding == encoding::utf_16be)
        {
            return encoding::utf_8;
        }

        // 3.
        if(meta_encoding == encoding::x_user_defined)
        {
            return encoding::windows_1252;
        }

        // 4,5,6.
        return meta_encoding;
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead:change-the-encoding
    encoding get_meta_encoding(GumboNode* root)
    {
        // find <head>
        GumboNode* head = nullptr;
        for(size_t i = 0; i < root->v.element.children.length; i++)
        {
            auto* node = static_cast<GumboNode*>(root->v.element.children.data[i]);
            if(node->type == GUMBO_NODE_ELEMENT && node->v.element.tag == GUMBO_TAG_HEAD)
            {
                head = node;
                break;
            }
        }
        if(!head)
        {
            return encoding::null;
        }

        // go through <meta> tags in <head>
        for(size_t i = 0; i < head->v.element.children.length; i++)
        {
            auto* node = static_cast<GumboNode*>(head->v.element.children.data[i]);
            if(node->type != GUMBO_NODE_ELEMENT || node->v.element.tag != GUMBO_TAG_META)
            {
                continue;
            }

            auto* charset    = gumbo_get_attribute(&node->v.element.attributes, "charset");
            auto* http_equiv = gumbo_get_attribute(&node->v.element.attributes, "http-equiv");
            auto* content    = gumbo_get_attribute(&node->v.element.attributes, "content");
            // 1. If the element has a charset attribute...
            if(charset)
            {
                auto encoding = get_encoding(charset->value);
                if(encoding != encoding::null)
                {
                    return encoding;
                }
            }
            // 2. Otherwise, if the element has an http-equiv attribute...
            else if(http_equiv && t_strcasecmp(http_equiv->value, "content-type") == 0 && content)
            {
                auto encoding = extract_encoding_from_meta_element(content->value);
                if(encoding != encoding::null)
                {
                    return encoding;
                }
            }
        }

        return encoding::null;
    }

    // substitute for gumbo_parse that handles encodings
    GumboOutput* document::parse_html(estring str)
    {
        // https://html.spec.whatwg.org/multipage/parsing.html#the-input-byte-stream
        encoding_sniffing_algorithm(str);
        // cannot store output in local variable because gumbo keeps pointers into it,
        // which will be accessed later in gumbo_tag_from_original_text
        if(str.encoding == encoding::utf_8 && str.confidence == confidence::certain)
        {
            m_text = std::move(static_cast<string&>(str));
        } else if(str.encoding == encoding::utf_8)
        {
            m_text = str;
        } else
        {
            decode(str, str.encoding, m_text);
        }

        // Gumbo does not support callbacks on node creation, so we cannot change encoding while parsing.
        // Instead, we parse entire file and then handle <meta> tags.

        // Using gumbo_parse_with_options to pass string length (m_text may contain NUL chars).
        auto options          = kGumboDefaultOptions;
        options.userdata      = m_container;
        options.maximum_nodes = m_maximum_nodes ? m_maximum_nodes + 1 : 0;
        options.maximum_depth = m_maximum_depth ? m_maximum_depth + 1 : 0;
        options.cancelled     = [](void* context) { return static_cast<document_container*>(context)->is_cancelled(); };
        GumboOutput* output   = gumbo_parse_with_options(&options, m_text.data(), m_text.size());
        if(m_container->is_cancelled())
        {
            return output;
        }

        if(str.confidence == confidence::certain || output->limit_exceeded || !output->root)
        {
            return output;
        }

        // Otherwise: confidence is tentative.

        // If valid HTML encoding is specified in <meta> tag...
        encoding meta_encoding = get_meta_encoding(output->root);
        if(meta_encoding != encoding::null)
        {
            // ...and it is different from currently used encoding...
            encoding new_encoding = adjust_meta_encoding(meta_encoding, str.encoding);
            if(new_encoding != str.encoding)
            {
                // ...reparse with the new encoding.
                gumbo_destroy_output(&kGumboDefaultOptions, output);
                m_text.clear();

                if(new_encoding == encoding::utf_8)
                {
                    m_text = str;
                } else
                {
                    decode(str, new_encoding, m_text);
                }
                output = gumbo_parse_with_options(&options, m_text.data(), m_text.size());
            }
        }

        return output;
    }

    void document::account_node()
    {
        m_container->check_cancelled();
        if(m_maximum_nodes && ++m_created_nodes > m_maximum_nodes)
        {
            throw rendering_cancelled{};
        }
    }
    void document::create_node(void* gnode, elements_list& elements, bool parseTextNode, bool process_root)
    {
        m_container->check_cancelled();
        auto* node = (GumboNode*) gnode;
        switch(node->type)
        {
        case GUMBO_NODE_ELEMENT:
            {
                if(process_root)
                {
                    account_node();
                    string_map      attrs;
                    GumboAttribute* attr = nullptr;
                    for(unsigned int i = 0; i < node->v.element.attributes.length; i++)
                    {
                        attr              = static_cast<GumboAttribute*>(node->v.element.attributes.data[i]);
                        attrs[attr->name] = attr->value;
                    }

                    element::ptr ret;
                    const char*  tag = gumbo_normalized_tagname(node->v.element.tag);
                    if(tag[0])
                    {
                        ret = create_element(tag, attrs);
                    } else
                    {
                        if(node->v.element.original_tag.data && node->v.element.original_tag.length)
                        {
                            std::string str;
                            gumbo_tag_from_original_text(&node->v.element.original_tag);
                            str.append(node->v.element.original_tag.data, node->v.element.original_tag.length);
                            ret = create_element(str.c_str(), attrs);
                        }
                    }
                    if(!strcmp(tag, "script"))
                    {
                        parseTextNode = false;
                    }
                    if(ret)
                    {
                        elements_list child;
                        for(unsigned int i = 0; i < node->v.element.children.length; i++)
                        {
                            child.clear();
                            create_node(static_cast<GumboNode*>(node->v.element.children.data[i]), child, parseTextNode,
                                        true);
                            std::for_each(child.begin(), child.end(),
                                          [&ret](element::ptr& el) { ret->appendChild(el); });
                        }
                        elements.push_back(ret);
                    }
                } else
                {
                    for(unsigned int i = 0; i < node->v.element.children.length; i++)
                    {
                        create_node(static_cast<GumboNode*>(node->v.element.children.data[i]), elements, parseTextNode,
                                    true);
                    }
                }
            }
            break;
        case GUMBO_NODE_TEXT:
            {
                if(!parseTextNode)
                {
                    account_node();
                    elements.push_back(std::make_shared<el_text>(node->v.text.text, shared_from_this()));
                } else
                {
                    m_container->split_text(
                        node->v.text.text,
                        [this, &elements](const char* text) {
                            account_node();
                            elements.push_back(std::make_shared<el_text>(text, shared_from_this()));
                        },
                        [this, &elements](const char* text) {
                            account_node();
                            elements.push_back(std::make_shared<el_space>(text, shared_from_this()));
                        });
                }
            }
            break;
        case GUMBO_NODE_CDATA:
            {
                account_node();
                element::ptr ret = std::make_shared<el_cdata>(shared_from_this());
                ret->set_data(node->v.text.text);
                elements.push_back(ret);
            }
            break;
        case GUMBO_NODE_COMMENT:
            {
                account_node();
                element::ptr ret = std::make_shared<el_comment>(shared_from_this());
                ret->set_data(node->v.text.text);
                elements.push_back(ret);
            }
            break;
        case GUMBO_NODE_WHITESPACE:
            {
                std::string str = node->v.text.text;
                for(size_t i = 0; i < str.length(); i++)
                {
                    account_node();
                    elements.push_back(std::make_shared<el_space>(str.substr(i, 1).c_str(), shared_from_this()));
                }
            }
            break;
        default:
            break;
        }
    }

    element::ptr document::create_element(const char* tag_name, const string_map& attributes)
    {
        element::ptr  newTag;
        document::ptr this_doc = shared_from_this();
        if(m_container)
        {
            newTag = m_container->create_element(tag_name, attributes, this_doc);
        }
        if(!newTag)
        {
            if(!strcmp(tag_name, "br"))
            {
                newTag = std::make_shared<el_break>(this_doc);
            } else if(!strcmp(tag_name, "p"))
            {
                newTag = std::make_shared<el_para>(this_doc);
            } else if(!strcmp(tag_name, "img"))
            {
                newTag = std::make_shared<el_image>(this_doc);
            } else if(!strcmp(tag_name, "table"))
            {
                newTag = std::make_shared<el_table>(this_doc);
            } else if(!strcmp(tag_name, "td") || !strcmp(tag_name, "th"))
            {
                newTag = std::make_shared<el_td>(this_doc);
            } else if(!strcmp(tag_name, "link"))
            {
                newTag = std::make_shared<el_link>(this_doc);
            } else if(!strcmp(tag_name, "title"))
            {
                newTag = std::make_shared<el_title>(this_doc);
            } else if(!strcmp(tag_name, "a"))
            {
                newTag = std::make_shared<el_anchor>(this_doc);
            } else if(!strcmp(tag_name, "tr"))
            {
                newTag = std::make_shared<el_tr>(this_doc);
            } else if(!strcmp(tag_name, "style"))
            {
                newTag = std::make_shared<el_style>(this_doc);
            } else if(!strcmp(tag_name, "base"))
            {
                newTag = std::make_shared<el_base>(this_doc);
            } else if(!strcmp(tag_name, "body"))
            {
                newTag = std::make_shared<el_body>(this_doc);
            } else if(!strcmp(tag_name, "div"))
            {
                newTag = std::make_shared<el_div>(this_doc);
            } else if(!strcmp(tag_name, "script"))
            {
                newTag = std::make_shared<el_script>(this_doc);
            } else if(!strcmp(tag_name, "font"))
            {
                newTag = std::make_shared<el_font>(this_doc);
            } else
            {
                newTag = std::make_shared<html_tag>(this_doc);
            }
        }

        if(newTag)
        {
            newTag->set_tagName(tag_name);
            for(const auto& attribute : attributes)
            {
                newTag->set_attr(attribute.first.c_str(), attribute.second.c_str());
            }
        }

        return newTag;
    }

    uint_ptr document::add_font(const font_description& descr, font_metrics* fm)
    {
        uint_ptr ret = 0;

        std::string key = descr.hash();

        if(m_fonts.find(key) == m_fonts.end())
        {
            font_item fi = {0, {}};

            fi.font      = m_container->create_font(descr, this, &fi.metrics);
            m_fonts[key] = fi;
            ret          = fi.font;
            if(fm)
            {
                *fm = fi.metrics;
            }
        }
        return ret;
    }

    uint_ptr document::get_font(const font_description& descr, font_metrics* fm)
    {
        if(descr.size == 0_px)
        {
            return 0;
        }

        auto key = descr.hash();

        auto el = m_fonts.find(key);

        if(el != m_fonts.end())
        {
            if(fm)
            {
                *fm = el->second.metrics;
            }
            return el->second.font;
        }
        return add_font(descr, fm);
    }

    pixel_t document::render(pixel_t max_width, render_type rt)
    {
        if(rt != render_fixed_only)
        {
            m_render_width = max_width;
        }
        pixel_t ret = 0_px;
        if(m_root && m_root_render)
        {
            position viewport;
            m_container->get_viewport(viewport);
            containing_block_context cb_context;
            cb_context.width         = max_width;
            cb_context.width.type    = containing_block_context::cbc_value_type_absolute;
            cb_context.height        = viewport.height;
            cb_context.height.type   = containing_block_context::cbc_value_type_absolute;
            cb_context.quirks_height = cb_context.height;
            cb_context.render_width  = cb_context.width;

            m_root_render->update_intrinsic_size();

            if(rt == render_fixed_only)
            {
                m_fixed_boxes.clear();
                m_root_render->render_positioned(rt);
            } else
            {
                ret = m_root_render->render(0_px, 0_px, cb_context, nullptr);
                if(m_root_render->fetch_positioned())
                {
                    m_fixed_boxes.clear();
                    m_root_render->render_positioned(rt);
                }
                m_size.width  = 0;
                m_size.height = 0;
                m_root_render->calc_document_size(m_size);
            }
        } else if(rt != render_fixed_only)
        {
            m_size = {};
            m_fixed_boxes.clear();
        }
        return ret;
    }

    void document::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip)
    {
        if(m_root && m_root_render)
        {
            m_root->draw(hdc, x, y, clip, m_root_render);
            m_root_render->draw_stacking_context(hdc, x, y, clip, true);
        }
    }

    pixel_t document::to_pixels(const css_length& val, const font_metrics& metrics, pixel_t size) const
    {
        if(val.is_predefined())
        {
            return 0_px;
        }
        if(val.is_expression())
        {
            auto resolved = val;
            resolved.resolve_units(
                [&](float value, css_units unit) { return to_pixels(css_length(value, unit), metrics, size).value(); });
            return resolved.calc_percent(size);
        }
        pixel_t ret;
        switch(val.units())
        {
        case css_units_percentage:
            ret = val.calc_percent(size);
            break;
        case css_units_em:
            ret = metrics.font_size * val.val();
            break;

        // https://drafts.csswg.org/css-values-4/#absolute-lengths
        case css_units_pt:
            ret = m_container->pt_to_px(val.val());
            break;
        case css_units_in:
            ret = m_container->pt_to_px(val.val() * 72); // 1in = 72pt
            break;
        case css_units_pc:
            ret = m_container->pt_to_px(val.val() * 12); // 1pc = (1/6)in = 12pt
            break;
        case css_units_cm:
            ret = m_container->pt_to_px(val.val() * 72 / 2.54f); // 1cm = (1/2.54)in = (72/2.54)pt
            break;
        case css_units_mm:
            ret = m_container->pt_to_px(val.val() * 72 / 2.54f / 10);
            break;

        case css_units_vw:
            ret = pixel_t(m_media.width.value() * val.val() / 100.0f);
            break;
        case css_units_vh:
            ret = pixel_t(m_media.height.value() * val.val() / 100.0f);
            break;
        case css_units_vmin:
            ret = pixel_t(std::min(m_media.height.value(), m_media.width.value()) * val.val() / 100.0f);
            break;
        case css_units_vmax:
            ret = pixel_t(std::max(m_media.height.value(), m_media.width.value()) * val.val() / 100.0f);
            break;
        case css_units_rem:
            ret = pixel_t(m_root->css().get_font_size().value() * val.val());
            break;
        case css_units_ex:
            ret = pixel_t(metrics.x_height.value() * val.val());
            break;
        case css_units_ch:
            ret = pixel_t(metrics.ch_width.value() * val.val());
            break;
        default:
            ret = pixel_t(val.val());
            break;
        }
        return ret;
    }

    void document::cvt_units(css_length& val, const font_metrics& metrics, pixel_t size) const
    {
        if(const auto* argument = val.fit_content_argument())
        {
            auto resolved = *argument;
            cvt_units(resolved, metrics, size);
            val.set_fit_content(resolved);
            return;
        }
        if(val.is_predefined())
        {
            return;
        }
        if(val.is_expression())
        {
            val.resolve_units(
                [&](float value, css_units unit) { return to_pixels(css_length(value, unit), metrics, size).value(); });
            return;
        }
        if(val.units() != css_units_percentage)
        {
            val.set_value(static_cast<float>(to_pixels(val, metrics, size)), css_units_px);
        }
    }

    pixel_t document::width() const
    {
        return m_size.width;
    }

    pixel_t document::height() const
    {
        return m_size.height;
    }

    void document::add_stylesheet(const char* str, const char* baseurl, const char* media)
    {
        if(str && str[0])
        {
            m_css.emplace_back(str, baseurl, media);
        }
    }

    bool document::on_mouse_over(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                 const std::function<void(const position&)>& redraw_box)
    {
        if(!m_root || !m_root_render)
        {
            return false;
        }

        element::ptr over_el = m_root_render->get_element_by_point(x, y, client_x, client_y, nullptr);

        bool state_was_changed = false;

        if(over_el != m_over_element)
        {
            if(m_over_element)
            {
                if(m_over_element->on_mouse_leave())
                {
                    m_container->on_mouse_event(m_over_element, mouse_event_leave);
                    state_was_changed = true;
                }
            }
            m_over_element = over_el;
        }

        std::string cursor;

        if(m_over_element)
        {
            if(m_over_element->on_mouse_over())
            {
                state_was_changed = true;
            }
            cursor = m_over_element->css().get_cursor();
        }

        m_container->set_cursor(cursor.c_str());

        if(state_was_changed)
        {
            m_container->on_mouse_event(m_over_element, mouse_event_enter);
            return update_styles(redraw_box);
        }
        return false;
    }

    std::vector<scroll_values> document::on_scroll(pixel_t dx, pixel_t dy, pixel_t x, pixel_t y, pixel_t client_x,
                                                   pixel_t client_y) const
    {
        if(dy == 0_px && dx == 0_px)
        {
            return {};
        }

        element::ptr vscroll_el;
        element::ptr hscroll_el;

        if(dy != 0_px)
        {
            vscroll_el = m_root_render->get_element_by_point(
                x, y, client_x, client_y,
                [dy](const std::shared_ptr<render_item>& el) -> bool { return el->is_v_scrollable(dy); });
        }

        if(dx != 0_px)
        {
            hscroll_el = m_root_render->get_element_by_point(
                x, y, client_x, client_y,
                [dx](const std::shared_ptr<render_item>& el) -> bool { return el->is_h_scrollable(dx); });
        }

        if(!vscroll_el && !hscroll_el)
        {
            return {};
        }

        if(vscroll_el == hscroll_el)
        {
            scroll_values sv;
            sv.dx         = hscroll_el->h_scroll(dx);
            sv.dy         = vscroll_el->v_scroll(dy);
            sv.scroll_box = hscroll_el->get_placement();
            return {sv};
        }

        std::vector<scroll_values> ret;
        ret.reserve(2);

        if(vscroll_el)
        {
            scroll_values sv;
            sv.dy         = vscroll_el->v_scroll(dy);
            sv.scroll_box = vscroll_el->get_placement();
            ret.push_back(sv);
        }

        if(hscroll_el)
        {
            scroll_values sv;
            sv.dx         = hscroll_el->h_scroll(dx);
            sv.scroll_box = hscroll_el->get_placement();
            ret.push_back(sv);
        }

        return ret;
    }

    bool document::on_mouse_leave(const std::function<void(const position&)>& redraw_box)
    {
        if(!m_root || !m_root_render)
        {
            return false;
        }
        if(m_over_element)
        {
            auto el        = m_over_element;
            m_over_element = nullptr;
            if(el->on_mouse_leave())
            {
                m_container->on_mouse_event(el, mouse_event_leave);
                return update_styles(redraw_box);
            }
        }
        return false;
    }

    bool document::on_lbutton_down(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                   const std::function<void(const position&)>& redraw_box)
    {
        if(!m_root || !m_root_render)
        {
            return false;
        }

        element::ptr over_el = m_root_render->get_element_by_point(x, y, client_x, client_y, nullptr);
        m_active_element     = over_el;

        bool state_was_changed = false;

        if(over_el != m_over_element)
        {
            if(m_over_element)
            {
                if(m_over_element->on_mouse_leave())
                {
                    m_container->on_mouse_event(m_over_element, mouse_event_leave);
                    state_was_changed = true;
                }
            }
            m_over_element = over_el;
            if(m_over_element)
            {
                if(m_over_element->on_mouse_over())
                {
                    state_was_changed = true;
                }
            }
        }

        std::string cursor;

        if(m_over_element)
        {
            if(m_over_element->on_lbutton_down())
            {
                state_was_changed = true;
            }
            cursor = m_over_element->css().get_cursor();
        }

        m_container->set_cursor(cursor.c_str());

        if(state_was_changed)
        {
            m_container->on_mouse_event(m_over_element, mouse_event_enter);
            return update_styles(redraw_box);
        }

        return false;
    }

    bool document::on_lbutton_up(pixel_t /*x*/, pixel_t /*y*/, pixel_t /*client_x*/, pixel_t /*client_y*/,
                                 const std::function<void(const position&)>& redraw_box)
    {
        if(!m_root || !m_root_render)
        {
            return false;
        }
        if(m_over_element)
        {
            if(m_over_element->on_lbutton_up(m_active_element == m_over_element))
            {
                return update_styles(redraw_box);
            }
        }
        return false;
    }

    bool document::on_button_cancel(const std::function<void(const position&)>& redraw_box)
    {
        m_active_element = nullptr;
        return on_mouse_leave(redraw_box);
    }

    void document::get_fixed_boxes(position::vector& fixed_boxes)
    {
        fixed_boxes = m_fixed_boxes;
    }

    void document::add_fixed_box(const position& pos)
    {
        m_fixed_boxes.push_back(pos);
    }

    size_t document::retained_font_bytes() const
    {
        size_t           bytes       = 0;
        constexpr size_t entry_bytes = sizeof(fonts_map::value_type) + sizeof(void*) * 4;
        for(const auto& font : m_fonts)
        {
            bytes += entry_bytes + font.first.capacity();
        }
        return bytes;
    }

    bool document::refresh_fonts(bool retain_fonts)
    {
        m_container->check_cancelled();
        // Font handles include the CSS family, size and emphasis. Pure point-size
        // changes can retain their bounded metrics/word caches for recent sizes.
        constexpr size_t maximum_retained_font_bytes = 64 * 1024;
        if(!retain_fonts || retained_font_bytes() > maximum_retained_font_bytes)
        {
            for(auto& font : m_fonts)
            {
                m_container->delete_font(font.second.font);
            }
            m_fonts.clear();
        }
        // A font-dependent media condition can change the cascade. Only that
        // case needs selector matching and generated-content reconstruction.
        for(auto& media : m_media_lists)
        {
            m_container->check_cancelled();
            media->refresh_units(shared_from_this());
        }
        if(media_changed())
        {
            return true;
        }
        // With an unchanged cascade, font metrics cannot change renderer types or
        // DOM membership. Keep the render tree, including its stable anchor paths.
        m_root->compute_styles();
        std::vector<std::shared_ptr<render_item>> pending{m_root_render};
        while(!pending.empty())
        {
            m_container->check_cancelled();
            auto node = std::move(pending.back());
            pending.pop_back();
            if(!node)
            {
                continue;
            }
            // Anonymous table/flow/flex boxes are absent from the source DOM.
            // Recompute their inherited metrics after their real source ancestors.
            if(node->src_el()->tag() == empty_id && !node->src_el()->is_text())
            {
                node->src_el()->compute_styles(false);
            }
            for(const auto& child : node->children())
            {
                pending.push_back(child);
            }
        }
        return false;
    }

    void document::discard_render_tree()
    {
        m_root_render.reset();
        m_tabular_elements.clear();
        m_fixed_boxes.clear();
        m_over_element = m_active_element = nullptr;
        std::vector<element::ptr> pending{m_root};
        while(!pending.empty())
        {
            auto node = std::move(pending.back());
            pending.pop_back();
            if(!node)
            {
                continue;
            }
            node->m_renders.clear();
            for(const auto& child : node->m_children)
            {
                pending.push_back(child);
            }
        }
    }
    void document::restore_render_tree()
    {
        if(m_root_render || !m_root)
        {
            return;
        }
        m_container->check_cancelled();
        m_root_render = m_root->create_render_item(nullptr);
        fix_tables_layout();
        if(m_root_render)
        {
            m_root_render = m_root_render->init();
        }
    }

    void document::restyle(const string& master_styles, bool reset_fonts)
    {
        m_master_style_text = master_styles;
        m_container->check_cancelled();
        m_root_render.reset();
        m_tabular_elements.clear();
        m_fixed_boxes.clear();
        m_over_element = m_active_element = nullptr;
        std::vector<element::ptr> pending{m_root};
        while(!pending.empty())
        {
            m_container->check_cancelled();
            auto node = std::move(pending.back());
            pending.pop_back();
            if(!node)
            {
                continue;
            }
            // Generated content belongs to styles, not to the source DOM. Remove
            // it before reapplying selectors so repeated updates stay bounded.
            node->m_children.erase(std::remove_if(node->m_children.begin(), node->m_children.end(),
                                                  [](const auto& child) {
                                                      return child->tag() == __tag_before_ ||
                                                             child->tag() == __tag_after_;
                                                  }),
                                   node->m_children.end());
            node->m_child_indexes_valid = false;
            if(node->m_auxiliary)
            {
                node->m_auxiliary->styles.clear();
            }
            node->m_renders.clear();
            if(node->m_auxiliary)
            {
                node->m_auxiliary->counters.clear();
            }
            if(auto tag = std::dynamic_pointer_cast<html_tag>(node))
            {
                tag->m_style.clear();
                tag->m_style_sources.clear();
            }
            for(const auto& child : node->m_children)
            {
                pending.push_back(child);
            }
        }
        if(reset_fonts)
        {
            for(auto& font : m_fonts)
            {
                m_container->delete_font(font.second.font);
            }
            m_fonts.clear();
        }
        m_master_css = css();
        m_styles     = css();
        m_user_css   = css();
        m_media_lists.clear();
        m_css.clear();
        auto self = shared_from_this();
        m_master_css.parse_css_stylesheet(master_styles, "", self);
        m_master_css.sort_selectors();
        m_user_css.parse_css_stylesheet(m_user_style_text, "", self);
        m_user_css.sort_selectors();
        m_container->get_media_features(m_media);
        m_root->apply_stylesheet(m_master_css);
        m_root->parse_attributes();
        for(const auto& source : m_css)
        {
            m_container->check_cancelled();
            media_query_list_list::ptr media;
            if(!source.media.empty())
            {
                media = make_shared<media_query_list_list>();
                media->add(parse_media_query_list(source.media, self));
            }
            m_styles.parse_css_stylesheet(source.text, source.baseurl, self, media);
        }
        m_styles.sort_selectors();
        update_media_lists(m_media);
        m_root->apply_stylesheet(m_styles);
        m_root->apply_stylesheet(m_user_css);
        m_root->compute_styles();
        m_root_render = m_root->create_render_item(nullptr);
        fix_tables_layout();
        if(m_root_render)
        {
            m_root_render = m_root_render->init();
        }
    }

    void document::rebuild_render_tree()
    {
        // Reuse scroll state for surviving boxes; layout will clamp it to their new sizes.
        std::map<element::ptr, std::shared_ptr<scroll_view>> scroll_views;
        const auto save_scroll_views = [&](const auto& self, const std::shared_ptr<render_item>& item) -> void {
            if(item->m_scroll_view)
            {
                scroll_views.emplace(item->src_el(), item->m_scroll_view);
            }
            for(const auto& child : item->children())
            {
                self(self, child);
            }
        };
        if(m_root_render)
        {
            save_scroll_views(save_scroll_views, m_root_render);
        }
        m_tabular_elements.clear();
        m_root_render.reset();
        if(!m_root)
        {
            return;
        }
        m_root->clear_renders();
        m_root_render = m_root->create_render_item(nullptr);
        fix_tables_layout();
        if(m_root_render)
        {
            m_root_render = m_root_render->init();
            if(!scroll_views.empty())
            {
                const auto restore_scroll_views = [&](const auto&                         self,
                                                      const std::shared_ptr<render_item>& item) -> void {
                    auto saved = scroll_views.find(item->src_el());
                    if(saved != scroll_views.end() &&
                       is_one_of(item->css().get_overflow(), overflow_scroll, overflow_auto))
                    {
                        item->m_scroll_view = saved->second;
                    }
                    for(const auto& child : item->children())
                    {
                        self(self, child);
                    }
                };
                restore_scroll_views(restore_scroll_views, m_root_render);
            }
        }
    }

    bool document::update_styles(const std::function<void(const position&)>& redraw_box)
    {
        bool       render_tree_changed = false;
        const bool changed             = m_root->find_styles_changes(redraw_box, &render_tree_changed);
        if(render_tree_changed)
        {
            const auto old_size = m_size;
            rebuild_render_tree();
            if(m_render_width)
            {
                render(*m_render_width);
                position viewport;
                container()->get_viewport(viewport);
                redraw_box(position(0_px, 0_px, std::max({old_size.width, m_size.width, viewport.right()}),
                                    std::max({old_size.height, m_size.height, viewport.bottom()})));
            }
        }
        return changed;
    }

    bool document::media_changed()
    {
        container()->get_media_features(m_media);
        if(update_media_lists(m_media))
        {
            // Reapply attribute styles and generated content as well as selectors.
            // refresh_styles() alone loses mapped table attributes and retains
            // obsolete pseudo elements across responsive breakpoints.
            restyle(m_master_style_text, false);
            return true;
        }
        return false;
    }

    bool document::lang_changed()
    {
        if(m_root)
        {
            std::string culture;
            container()->get_language(m_lang, culture);
            if(!culture.empty())
            {
                m_culture = m_lang + '-' + culture;
            } else
            {
                m_culture.clear();
            }
            m_root->refresh_styles();
            m_root->compute_styles();
            rebuild_render_tree();
            return true;
        }
        return false;
    }

    // Apply media features (determine which selectors are active).
    bool document::update_media_lists(const media_features& features)
    {
        bool update_styles = false;
        for(auto& media_list : m_media_lists)
        {
            if(media_list->apply_media_features(features))
            {
                update_styles = true;
            }
        }
        return update_styles;
    }

    void document::add_media_list(const media_query_list_list::ptr& list)
    {
        if(list && !contains(m_media_lists, list))
        {
            m_media_lists.push_back(list);
        }
    }

    void document::fix_tables_layout()
    {
        for(const auto& el_ptr : m_tabular_elements)
        {
            switch(el_ptr->src_el()->css().get_display())
            {
            case display_inline_table:
            case display_table:
                fix_table_children(el_ptr, display_table_row_group, "table-row-group");
                break;
            case display_table_footer_group:
            case display_table_row_group:
            case display_table_header_group:
                {
                    auto parent = el_ptr->parent();
                    if(parent)
                    {
                        if(parent->src_el()->css().get_display() != display_inline_table)
                        {
                            fix_table_parent(el_ptr, display_table, "table");
                        }
                    }
                    fix_table_children(el_ptr, display_table_row, "table-row");
                }
                break;
            case display_table_row:
                fix_table_parent(el_ptr, display_table_row_group, "table-row-group");
                fix_table_children(el_ptr, display_table_cell, "table-cell");
                break;
            case display_table_cell:
                fix_table_parent(el_ptr, display_table_row, "table-row");
                break;
            // TODO: make table layout fix for table-caption, table-column etc. elements
            case display_table_caption:
            case display_table_column:
            case display_table_column_group:
            default:
                break;
            }
        }
    }

    void document::fix_table_children(const std::shared_ptr<render_item>& el_ptr, style_display disp,
                                      const char* disp_str)
    {
        std::list<std::shared_ptr<render_item>> tmp;
        auto                                    first_iter = el_ptr->children().begin();
        auto                                    cur_iter   = el_ptr->children().begin();

        auto flush_elements = [&]() {
            element::ptr annon_tag = std::make_shared<html_tag>(el_ptr->src_el(), std::string("display:") + disp_str);
            std::shared_ptr<render_item> annon_ri;
            if(annon_tag->css().get_display() == display_table_cell)
            {
                annon_tag->set_tagName("table_cell");
                annon_ri = std::make_shared<render_item_block>(annon_tag);
            } else if(annon_tag->css().get_display() == display_table_row)
            {
                annon_ri = std::make_shared<render_item_table_row>(annon_tag);
            } else
            {
                annon_ri = std::make_shared<render_item_table_part>(annon_tag);
            }
            for(const auto& el : tmp)
            {
                annon_ri->add_child(el);
            }
            // add annon item as tabular for future processing
            add_tabular(annon_ri);
            annon_ri->parent(el_ptr);
            first_iter = el_ptr->children().insert(first_iter, annon_ri);
            cur_iter   = std::next(first_iter);
            while(cur_iter != el_ptr->children().end() && (*cur_iter)->parent() != el_ptr)
            {
                cur_iter = el_ptr->children().erase(cur_iter);
            }
            first_iter = cur_iter;
            tmp.clear();
        };

        while(cur_iter != el_ptr->children().end())
        {
            if((*cur_iter)->src_el()->css().get_display() != disp)
            {
                if(!(*cur_iter)->src_el()->is_table_skip() || ((*cur_iter)->src_el()->is_table_skip() && !tmp.empty()))
                {
                    if(disp != display_table_row_group ||
                       !is_one_of((*cur_iter)->src_el()->css().get_display(), display_table_caption,
                                  display_table_column, display_table_column_group))
                    {
                        if(tmp.empty())
                        {
                            first_iter = cur_iter;
                        }
                        tmp.push_back((*cur_iter));
                    }
                }
                cur_iter++;
            } else if(!tmp.empty())
            {
                flush_elements();
            } else
            {
                cur_iter++;
            }
        }
        if(!tmp.empty())
        {
            flush_elements();
        }
    }

    void document::fix_table_parent(const std::shared_ptr<render_item>& el_ptr, style_display disp,
                                    const char* disp_str)
    {
        auto parent = el_ptr->parent();

        if(parent->src_el()->css().get_display() != disp)
        {
            auto this_element = std::find_if(parent->children().begin(), parent->children().end(),
                                             [&](const std::shared_ptr<render_item>& el) { return el == el_ptr; });
            if(this_element != parent->children().end())
            {
                style_display el_disp = el_ptr->src_el()->css().get_display();
                auto          first   = this_element;
                auto          last    = this_element;
                auto          cur     = this_element;

                // find first element with same display
                while(true)
                {
                    if(cur == parent->children().begin())
                    {
                        break;
                    }
                    cur--;
                    if((*cur)->src_el()->is_table_skip() || (*cur)->src_el()->css().get_display() == el_disp)
                    {
                        first = cur;
                    } else
                    {
                        break;
                    }
                }

                // find last element with same display
                cur = this_element;
                while(true)
                {
                    cur++;
                    if(cur == parent->children().end())
                    {
                        break;
                    }

                    if((*cur)->src_el()->is_table_skip() || (*cur)->src_el()->css().get_display() == el_disp)
                    {
                        last = cur;
                    } else
                    {
                        break;
                    }
                }

                // extract elements with the same display and wrap them with anonymous object
                element::ptr annon_tag =
                    std::make_shared<html_tag>(parent->src_el(), std::string("display:") + disp_str);
                std::shared_ptr<render_item> annon_ri;
                if(annon_tag->css().get_display() == display_table ||
                   annon_tag->css().get_display() == display_inline_table)
                {
                    annon_ri = std::make_shared<render_item_table>(annon_tag);
                } else if(annon_tag->css().get_display() == display_table_row)
                {
                    annon_ri = std::make_shared<render_item_table_row>(annon_tag);
                } else
                {
                    annon_ri = std::make_shared<render_item_table_part>(annon_tag);
                }
                std::for_each(first, std::next(last, 1),
                              [&annon_ri](std::shared_ptr<render_item>& el) { annon_ri->add_child(el); });
                first = parent->children().erase(first, std::next(last));
                parent->children().insert(first, annon_ri);
                add_tabular(annon_ri);
                annon_ri->parent(parent);
            }
        }
    }

    void document::append_children_from_string(element& parent, const char* str, bool replace_existing)
    {
        // parent must belong to this document
        if(parent.get_document().get() != this)
        {
            return;
        }

        GumboOptions opts = kGumboDefaultOptions;
        // This is require to prevent creating html, head, body tags around the fragment
        // Although Gumbo always creates html tag anyway. We have to ignore it in create_node.
        opts.fragment_context = GUMBO_TAG_BODY;
        // parse document into GumboOutput
        GumboOutput* output = gumbo_parse_with_options(&opts, str, strlen(str));

        // Create litehtml::elements.
        elements_list child_elements;
        // Create elements excluding the root node
        create_node(output->root, child_elements, true, false);

        // Destroy GumboOutput
        gumbo_destroy_output(&kGumboDefaultOptions, output);

        if(replace_existing)
        {
            parent.clearRecursive();
        }

        // Let's process created elements tree
        for(const auto& child : child_elements)
        {
            // Add the child element to parent
            parent.appendChild(child);

            // apply master CSS
            child->apply_stylesheet(m_master_css);

            // parse elements attributes
            child->parse_attributes();

            // Apply parsed styles.
            child->apply_stylesheet(m_styles);

            // Apply user styles if any
            child->apply_stylesheet(m_user_css);

            // Initialize m_css
            child->compute_styles();
        }
        rebuild_render_tree();
    }

    void document::dump(dumper& cout)
    {
        if(m_root_render)
        {
            m_root_render->dump(cout);
        }
    }

} // namespace litehtml

int litehtml::document::cached_select(const std::shared_ptr<element>& node, const css_selector& selector,
                                      bool apply_pseudo)
{
    container()->check_cancelled();
    if(!m_matching_styles)
    {
        return node->select(selector, apply_pseudo);
    }
    // Only immutable ancestor predicates enter this cache. Sibling, structural,
    // interaction and generated-content selectors retain their full evaluator.
    for(auto term = &selector; term; term = term->m_left.get())
    {
        if(term->m_left && term->m_combinator != combinator_child && term->m_combinator != combinator_descendant)
        {
            return node->select(selector, apply_pseudo);
        }
        for(const auto& attr : term->m_right.m_attrs)
        {
            if(attr.type == select_pseudo_class || attr.type == select_pseudo_element)
            {
                return node->select(selector, apply_pseudo);
            }
        }
    }
    const selector_match_key key{node.get(), &selector};
    const auto               found = m_selector_matches.find(key);
    if(found != m_selector_matches.end())
    {
        ++m_selector_hits;
        return found->second;
    }
    const int        result                 = node->select(selector, apply_pseudo);
    constexpr size_t maximum_selector_bytes = 2 * 1024 * 1024;
    if(m_selector_matches.size() < maximum_selector_bytes / selector_entry_bytes)
    {
        m_selector_matches.emplace(key, result);
    }
    return result;
}
