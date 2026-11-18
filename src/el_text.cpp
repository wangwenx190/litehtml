#include "html.h"
#include "el_text.h"
#include "render_item.h"
#include "document_container.h"

litehtml::el_text::el_text(const char* text, const document::ptr& doc) :
    element(doc)
{
    if(text)
    {
        m_text = text;
    }
    m_literal_text = nullptr;
    m_draw_spaces  = true;
    // Styles are prepared after the source DOM; avoid a full property object
    // per word while constructing the tree.
}

void litehtml::el_text::get_content_size(size& sz, pixel_t /*max_width*/)
{
    if(!m_measured)
    {
        font_metrics fm;
        uint_ptr     font = 0;
        if(parent())
        {
            font = parent()->css().get_font();
            fm   = parent()->css().get_font_metrics();
        }
        if(is_break() || !font)
        {
            m_size.height = 0;
            m_size.width  = 0;
        } else
        {
            m_size.height = fm.height;
            m_size.width  = get_document()->container()->text_width(display_text(), font);
        }
        m_measured = true;
    }
    sz = m_size;
}

void litehtml::el_text::get_text(std::string& text) const
{
    text += m_text;
}

void litehtml::el_text::compute_styles(bool /*recursive*/)
{
    element::ptr el_parent = parent();
    const bool   reuse     = get_document()->reuse_computed_styles();
    auto         shared    = reuse && el_parent ? el_parent->text_child_style() : nullptr;
    if(shared)
    {
        m_css = std::move(shared);
    } else
    {
        element::ptr el_parent = parent();
        if(el_parent)
        {
            css_w().line_height() = el_parent->css().line_height();
            css_w().set_font(el_parent->css().get_font());
            css_w().set_font_metrics(el_parent->css().get_font_metrics());
            css_w().set_white_space(el_parent->css().get_white_space());
            css_w().set_text_transform(el_parent->css().get_text_transform());
            css_w().set_visibility(el_parent->css().get_visibility());
        }
        css_w().set_display(display_inline_text);
        css_w().set_float(float_none);

        element::ptr positioned = parent();
        while(positioned && positioned->css().get_display() == display_inline)
        {
            if(positioned->css().get_position() == element_position_relative)
            {
                css_w().set_offsets(positioned->css().get_offsets());
                css_w().set_position(element_position_relative);
                break;
            }
            positioned = positioned->parent();
        }
        if(positioned)
        {
            css_w().set_position(element_position_static);
        }
        // Sibling words share their parent's derived text properties. Layout
        // writes still detach through css_w(); the next parent style pass
        // revokes this exact cache before children inherit new typography.
        if(reuse && el_parent)
        {
            el_parent->text_child_style(m_css);
        }
    }

    if(css().get_text_transform() != text_transform_none)
    {
        m_transformed_text = std::make_unique<string>(m_text);
        m_literal_text     = nullptr;

        get_document()->container()->transform_text(*m_transformed_text, css().get_text_transform());
    } else
    {
        m_transformed_text.reset();
        m_literal_text = nullptr;
    }

    if(is_white_space())
    {
        m_literal_text = " ";
        m_transformed_text.reset();
    } else
    {
        if(m_text == "\t")
        {
            m_literal_text = "    ";
            m_transformed_text.reset();
        }
        if(m_text == "\n" || m_text == "\r")
        {
            m_literal_text = "";
            m_transformed_text.reset();
        }
    }

    // Measure words only when layout demands them, not while styling the full DOM.
    m_measured    = false;
    m_draw_spaces = css().get_font_metrics().draw_spaces;
}

void litehtml::el_text::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                             const std::shared_ptr<render_item>& ri)
{
    if(is_white_space() && !m_draw_spaces)
    {
        return;
    }

    position pos  = ri->pos();
    pos.x        += x;
    pos.y        += y;
    pos.round();

    if(pos.does_intersect(clip))
    {
        element::ptr el_parent = parent();
        if(el_parent)
        {
            document::ptr doc = get_document();

            uint_ptr font = el_parent->css().get_font();
            if(font)
            {
                web_color color = el_parent->css().get_color();
                doc->container()->draw_text(hdc, display_text(), font, color, pos);
            }
        }
    }
}

std::string litehtml::el_text::dump_get_name()
{
    return "text: \"" + get_escaped_string(m_text) + "\"";
}

std::vector<std::tuple<std::string, std::string>> litehtml::el_text::dump_get_attrs()
{
    return {};
}
