#ifndef LITEHTML_DOCUMENT_H
#define LITEHTML_DOCUMENT_H

#include <string_view>

#include "stylesheet.h"
#include "css_properties.h"
#include "types.h"
#include "encodings.h"
#include "font_description.h"
#include "master_css.h"
#include "types.h"

#include <functional>
#include <optional>
#include <vector>

using GumboOutput = struct GumboInternalOutput;

namespace litehtml
{
    struct css_text
    {
        using vector = std::vector<css_text>;

        std::string text;
        std::string baseurl;
        std::string media;

        css_text() = default;

        css_text(const char* txt, const char* url, const char* media_str) :
            text(txt ? txt : ""),
            baseurl(url ? url : ""),
            media(media_str ? media_str : "")
        {
        }
    };

    class dumper
    {
      public:
        virtual ~dumper()                                                        = default;
        virtual void begin_node(const std::string& descr)                        = 0;
        virtual void end_node()                                                  = 0;
        virtual void begin_attrs_group(const std::string& descr)                 = 0;
        virtual void end_attrs_group()                                           = 0;
        virtual void add_attr(const std::string& name, const std::string& value) = 0;
    };

    class html_tag;
    class render_item;

    class document : public std::enable_shared_from_this<document>
    {
      public:
        using ptr      = std::shared_ptr<document>;
        using weak_ptr = std::weak_ptr<document>;

      private:
        std::shared_ptr<element>                m_root;
        std::shared_ptr<render_item>            m_root_render;
        document_container*                     m_container;
        fonts_map                               m_fonts;
        css_text::vector                        m_css;
        litehtml::css                           m_styles;
        litehtml::web_color                     m_def_color;
        litehtml::css                           m_master_css;
        litehtml::css                           m_user_css;
        litehtml::size                          m_size;
        std::optional<pixel_t>                  m_render_width;
        position::vector                        m_fixed_boxes;
        std::shared_ptr<element>                m_over_element;
        std::shared_ptr<element>                m_active_element;
        std::list<std::shared_ptr<render_item>> m_tabular_elements;
        media_query_list_list::vector           m_media_lists;
        media_features                          m_media;
        string                                  m_lang;
        string                                  m_culture;
        string                                  m_text;
        size_t                                  m_maximum_nodes = 0;
        size_t                                  m_maximum_depth = 0;
        size_t                                  m_created_nodes = 0;
        string                                  m_user_style_text;
        string                                  m_master_style_text;
        struct computed_style
        {
            std::shared_ptr<css_properties> value;
            std::uint64_t                   identity;
        };
        std::map<string, computed_style> m_computed_styles;
        std::uint64_t                    m_computed_sequence     = 0;
        std::uint64_t                    m_computed_hits         = 0;
        size_t                           m_computed_charge       = 0;
        bool                             m_reuse_computed_styles = true;
        struct selector_match_key
        {
            const element*      node;
            const css_selector* selector;
            bool                operator<(const selector_match_key& other) const
            {
                if(node != other.node)
                {
                    return std::less<const element*>{}(node, other.node);
                }
                return std::less<const css_selector*>{}(selector, other.selector);
            }
        };
        std::map<selector_match_key, int> m_selector_matches;
        std::uint64_t                     m_selector_hits        = 0;
        std::uint64_t                     m_flow_checkpoint_hits = 0;
        std::uint64_t                     m_sibling_index_builds = 0;
        bool                              m_matching_styles      = false;
        friend class html_tag;
        document_mode m_mode      = no_quirks_mode;
        bool          m_finalized = false;

      public:
        class selector_scope
        {
            document& doc;
            bool      previous;

          public:
            explicit selector_scope(document& value) :
                doc(value),
                previous(value.m_matching_styles)
            {
                if(!previous)
                {
                    doc.m_selector_matches.clear();
                }
                doc.m_matching_styles = true;
            }
            ~selector_scope()
            {
                doc.m_matching_styles = previous;
            }
        };
        void record_sibling_index()
        {
            ++m_sibling_index_builds;
        }
        std::uint64_t sibling_index_builds() const
        {
            return m_sibling_index_builds;
        }
        void record_flow_checkpoint()
        {
            ++m_flow_checkpoint_hits;
        }
        std::uint64_t flow_checkpoint_hits() const
        {
            return m_flow_checkpoint_hits;
        }
        int cached_select(const std::shared_ptr<element>& node, const css_selector& selector, bool apply_pseudo);
        std::uint64_t selector_cache_hits() const
        {
            return m_selector_hits;
        }
        document(document_container* objContainer);
        virtual ~document();

        document_container* container() const
        {
            return m_container;
        }
        document_mode mode() const
        {
            return m_mode;
        }
        void      set_document_mode(document_mode mode);
        void      finalize_from_external_root(const std::shared_ptr<element>& root, const std::string& master_styles,
                                              const std::string& user_styles);
        uint_ptr  get_font(const font_description& descr, font_metrics* fm);
        pixel_t   render(pixel_t max_width, render_type rt = render_all);
        void      draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip);
        web_color get_def_color() const
        {
            return m_def_color;
        }
        void                         cvt_units(css_length& val, const font_metrics& metrics, pixel_t size) const;
        pixel_t                      to_pixels(const css_length& val, const font_metrics& metrics, pixel_t size) const;
        pixel_t                      width() const;
        pixel_t                      height() const;
        pixel_t                      content_width() const;
        pixel_t                      content_height() const;
        void                         add_stylesheet(const char* str, const char* baseurl, const char* media);
        bool                         on_mouse_over(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                                   const std::function<void(const position&)>& redraw_box);
        std::vector<scroll_values>   on_scroll(pixel_t dx, pixel_t dy, pixel_t x, pixel_t y, pixel_t client_x,
                                               pixel_t client_y) const;
        bool                         on_lbutton_down(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                                     const std::function<void(const position&)>& redraw_box);
        bool                         on_lbutton_up(pixel_t x, pixel_t y, pixel_t client_x, pixel_t client_y,
                                                   const std::function<void(const position&)>& redraw_box);
        bool                         on_button_cancel(const std::function<void(const position&)>& redraw_box);
        bool                         on_mouse_leave(const std::function<void(const position&)>& redraw_box);
        std::shared_ptr<element>     create_element(const char* tag_name, const string_map& attributes);
        std::shared_ptr<element>     root();
        std::shared_ptr<render_item> root_render();
        void                         get_fixed_boxes(position::vector& fixed_boxes);
        void                         add_fixed_box(const position& pos);
        void                         add_media_list(const media_query_list_list::ptr& list);
        bool                         media_changed();
        // Rebuild style/render state while retaining the parsed source DOM.
        // The caller owns exclusive access and discards a cancelled document.
        void restyle(const string& master_styles, bool reset_fonts);
        // Host typography changes retain matched declarations and generated
        // content. Recompute inherited metrics and rebuild only render state.
        bool refresh_fonts(bool retain_fonts = false);
        // Exclusive cache compaction preserves source, declarations and fonts.
        void          discard_render_tree();
        void          restore_render_tree();
        std::uint64_t computed_style_hits() const
        {
            return m_computed_hits;
        }
        bool reuse_computed_styles() const
        {
            return m_reuse_computed_styles;
        }
        static constexpr size_t selector_entry_bytes = sizeof(void*) * 6 + sizeof(int);
        size_t                  retained_font_bytes() const;
        size_t                  computed_style_bytes() const
        {
            return m_computed_charge + m_selector_matches.size() * selector_entry_bytes + retained_font_bytes();
        }
        bool                           lang_changed();
        bool                           match_lang(const std::string& lang);
        void                           add_tabular(const std::shared_ptr<render_item>& el);
        std::shared_ptr<const element> get_over_element() const
        {
            return m_over_element;
        }

        void append_children_from_string(element& parent, const char* str, bool replace_existing);
        void dump(dumper& cout);

        // see doc/document_createFromString.txt
        static document::ptr createFromString(estring str, document_container* container,
                                              const string& master_styles = litehtml::master_css,
                                              const string& user_styles   = "",
                                              const std::function<bool(const document::ptr&)>& first_document = {},
                                              size_t initial_blocks = 0, size_t maximum_nodes = 0,
                                              size_t maximum_depth = 0, bool reuse_computed_styles = true,
                                              std::string_view validated_utf8 = {}, std::string_view initial_text = {},
                                              int initial_occurrence = -1);

      private:
        uint_ptr add_font(const font_description& descr, font_metrics* fm);

        GumboOutput*         parse_html(estring str);
        static document::ptr from_gumbo(GumboOutput* output, document_container* container, const string& master_styles,
                                        const string& user_styles, bool reuse_computed_styles, size_t maximum_nodes,
                                        size_t maximum_depth);
        void                 create_node(void* gnode, elements_list& elements, bool parseTextNode, bool process_root);
        void                 account_node();
        bool                 update_media_lists(const media_features& features);
        bool                 update_styles(const std::function<void(const position&)>& redraw_box);
        void                 rebuild_render_tree();
        void                 fix_tables_layout();
        void fix_table_children(const std::shared_ptr<render_item>& el_ptr, style_display disp, const char* disp_str);
        void fix_table_parent(const std::shared_ptr<render_item>& el_ptr, style_display disp, const char* disp_str);
    };

    inline std::shared_ptr<element> document::root()
    {
        return m_root;
    }

    inline std::shared_ptr<render_item> document::root_render()
    {
        return m_root_render;
    }

    inline void document::add_tabular(const std::shared_ptr<render_item>& el)
    {
        m_tabular_elements.push_back(el);
    }

    inline bool document::match_lang(const std::string& lang)
    {
        return lang == m_lang || lang == m_culture;
    }
} // namespace litehtml

#endif // LITEHTML_DOCUMENT_H
