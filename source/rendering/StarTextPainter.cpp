#include "StarTextPainter.hpp"
#include "StarJsonExtra.hpp"
#include <regex>

namespace Star {

TextPositioning::TextPositioning() {
  pos = Vec2F();
  hAnchor = HorizontalAnchor::LeftAnchor;
  vAnchor = VerticalAnchor::BottomAnchor;
}

TextPositioning::TextPositioning(Vec2F pos, HorizontalAnchor hAnchor, VerticalAnchor vAnchor,
    Maybe<unsigned> wrapWidth, Maybe<unsigned> charLimit)
  : pos(pos), hAnchor(hAnchor), vAnchor(vAnchor), wrapWidth(wrapWidth), charLimit(charLimit) {}

TextPositioning::TextPositioning(Json const& v) {
  pos = v.opt("position").apply(jsonToVec2F).value();
  hAnchor = HorizontalAnchorNames.getLeft(v.getString("horizontalAnchor", "left"));
  vAnchor = VerticalAnchorNames.getLeft(v.getString("verticalAnchor", "top"));
  wrapWidth = v.optUInt("wrapWidth");
  charLimit = v.optUInt("charLimit");
}

Json TextPositioning::toJson() const {
  return JsonObject{
    {"position", jsonFromVec2F(pos)},
    {"horizontalAnchor", HorizontalAnchorNames.getRight(hAnchor)},
    {"verticalAnchor", VerticalAnchorNames.getRight(vAnchor)},
    {"wrapWidth", jsonFromMaybe(wrapWidth)}
  };
}

TextPositioning TextPositioning::translated(Vec2F translation) const {
  return {pos + translation, hAnchor, vAnchor, wrapWidth, charLimit};
}

TextPainter::TextPainter(RendererPtr renderer, TextureGroupPtr textureGroup)
  : m_renderer(renderer),
    m_fontTextureGroup(textureGroup),
    m_defaultRenderSettings(),
    m_renderSettings(),
    m_savedRenderSettings() {
  reloadFonts();
  m_reloadTracker = make_shared<TrackerListener>();
  Root::singleton().registerReloadListener(m_reloadTracker);
}

RectF TextPainter::renderText(StringView s, TextPositioning const& position) {
  RectF rect;
  if (position.charLimit) {
    unsigned charLimit = *position.charLimit;
    rect = doRenderText(s, position, true, &charLimit);
  } else {
    rect = doRenderText(s, position, true, nullptr);
  }
  renderPrimitives();
  return rect;
}

RectF TextPainter::renderLine(StringView s, TextPositioning const& position) {
  RectF rect;
  if (position.charLimit) {
    unsigned charLimit = *position.charLimit;
    rect = doRenderLine(s, position, true, &charLimit);
  } else {
    rect = doRenderLine(s, position, true, nullptr);
  }
  renderPrimitives();
  return rect;
}

RectF TextPainter::renderGlyph(String::Char c, TextPositioning const& position) {
  auto rect = doRenderGlyph(c, position, true);
  renderPrimitives();
  return rect;
}

RectF TextPainter::determineTextSize(StringView s, TextPositioning const& position) {
  return doRenderText(s, position, false, nullptr);
}

RectF TextPainter::determineLineSize(StringView s, TextPositioning const& position) {
  return doRenderLine(s, position, false, nullptr);
}

RectF TextPainter::determineGlyphSize(String::Char c, TextPositioning const& position) {
  return doRenderGlyph(c, position, false);
}

int TextPainter::glyphWidth(String::Char c) {
  return m_fontTextureGroup.glyphWidth(c, m_renderSettings.fontSize);
}

int TextPainter::stringWidth(StringView s, unsigned charLimit) {
  if (s.empty())
    return 0;

  String font = m_renderSettings.font, setFont = font;
  m_fontTextureGroup.switchFont(font);

  Text::CommandsCallback commandsCallback = [&](StringView commands) {
    commands.forEachSplitView(",", [&](StringView command, size_t, size_t) {
      if (command == "reset") {
        m_fontTextureGroup.switchFont(font = setFont);
      } else if (command == "set") {
        setFont = font;
      } else if (command.beginsWith("font=")) {
        m_fontTextureGroup.switchFont(font = command.substr(5));
      }
    });
    return true;
  };

  int width = 0;
  Text::TextCallback textCallback = [&](StringView text) {
    for (String::Char c : text) {
      width += glyphWidth(c);
      if (charLimit && --charLimit == 0)
        return false;
    }
    return true;
  };

  Text::processText(s, textCallback, commandsCallback);

  return width;
}

bool TextPainter::processWrapText(StringView text, unsigned* wrapWidth, WrapTextCallback textFunc) {
  String font = m_renderSettings.font, setFont = font;
  m_fontTextureGroup.switchFont(font);
  unsigned lines = 0;

  auto it = text.begin();
  auto end = text.end();

  unsigned linePixelWidth = 0; // How wide is this line so far
  unsigned lineCharSize = 0; // how many characters in this line ?

  auto escIt = end;
  unsigned splitWidth = 0; // How wide was the string there ?

  auto lineStartIt = it; // Where does this line start ?
  auto splitIt = end; // != end if we last saw a place to split the string

  auto slice = [](StringView::const_iterator a, StringView::const_iterator b) -> StringView {
    const char* aPtr = &*a.base();
    return StringView(aPtr, &*b.base() - aPtr);
  };

  while (it != end) {
    auto character = *it;

    if (Text::isEscapeCode(character))
      escIt = it;

    if (escIt != end) {
      if (character == Text::EndEsc) {
        StringView inner = slice(++escIt, it);
        inner.forEachSplitView(",", [&](StringView command, size_t, size_t) {
          if (command == "reset") {
            m_fontTextureGroup.switchFont(font = setFont);
          } else if (command == "set") {
            setFont = font;
          } else if (command.beginsWith("font=")) {
            m_fontTextureGroup.switchFont(font = command.substr(5));
          }
        });
        escIt = end;
      }
      lineCharSize++;
    } else {
      lineCharSize++; // assume at least one character if we get here.

      // is this a linefeed / cr / whatever that forces a line split ?
      if (character == '\n' || character == '\v') {
        // knock one off the end because we don't render the CR
        if (!textFunc(slice(lineStartIt, it), lines++))
          return false;

        lineStartIt = it;
        ++lineStartIt;
        // next line starts after the CR with no characters in it and no known splits.
        lineCharSize = linePixelWidth = 0;
        splitIt = end;
      } else {
        int charWidth = glyphWidth(character);
        // is it a place where we might want to split the line ?
        if (character == ' ' || character == '\t') {
          splitIt = it;
          splitWidth = linePixelWidth + charWidth; // the width of the string at
          // the split point, i.e. after the space.
        }

        // would the line be too long if we render this next character ?
        if (wrapWidth && (linePixelWidth + charWidth) > *wrapWidth) {
          // did we find somewhere to split the line ?
          if (splitIt != end) {
            if (!textFunc(slice(lineStartIt, splitIt), lines++))
              return false;
            unsigned stringWidth = linePixelWidth - splitWidth;
            linePixelWidth = stringWidth + charWidth; // and is as wide as the bit after the space.
            lineStartIt = ++splitIt;
            splitIt = end;
          } else {
            if (!textFunc(slice(lineStartIt, it), lines++))
              return false;
            lineStartIt = it; // include that character on the next line. 
            lineCharSize = 1;            // next line has that character in
            linePixelWidth = charWidth; // and is as wide as that character
          }
        } else {
          linePixelWidth += charWidth;
        }
      }
    }

    ++it;
  };

  // if we hit the end of the string before hitting the end of the line.
  if (lineCharSize > 0)
    return textFunc(slice(lineStartIt, end), lines);

  return true;
}

List<StringView> TextPainter::wrapTextViews(StringView s, Maybe<unsigned> wrapWidth) {
  List<StringView> views = {};
  auto last = views.end();
  unsigned curLine = 0;
  TextPainter::WrapTextCallback textCallback = [&](StringView text, unsigned line) {
    if (line == curLine && last != views.end() && last->end() == text.begin()) {
      *last = StringView(last->utf8Ptr(), last->utf8Size() + text.utf8Size());
    } else {
      last = views.insert(views.end(), text);
      curLine = line;
    }
    return true;
  };

  processWrapText(s, wrapWidth.ptr(), textCallback);

  return views;
}

StringList TextPainter::wrapText(StringView s, Maybe<unsigned> wrapWidth) {
  StringList result;

  String current;
  int lastLine = 0;
  TextPainter::WrapTextCallback textCallback = [&](StringView text, unsigned line) {
    if (lastLine != line) {
      result.append(std::move(current));
      lastLine = line;
    }
    current += text;
    return true;
  };

  processWrapText(s, wrapWidth.ptr(), textCallback);

  if (!current.empty())
    result.append(std::move(current));

  return result;
};

unsigned TextPainter::fontSize() const {
  return m_renderSettings.fontSize;
}

void TextPainter::setFontSize(unsigned size) {
  m_renderSettings.fontSize = size;
}

void TextPainter::setLineSpacing(float lineSpacing) {
  m_renderSettings.lineSpacing = lineSpacing;
}

void TextPainter::setMode(FontMode mode) {
  m_renderSettings.shadow = fontModeToColor(mode).toRgba();
}

void TextPainter::setFontColor(Vec4B color) {
  m_renderSettings.color = std::move(color);
}

void TextPainter::setProcessingDirectives(StringView directives, bool back) {
  Directives& target = back ? m_renderSettings.backDirectives : m_renderSettings.directives;
  modifyDirectives(target = String(directives));
}

void TextPainter::setFont(String const& font) {
  m_renderSettings.font = font;
}

TextStyle& TextPainter::setTextStyle(TextStyle const& textStyle) {
  TextStyle& style = m_renderSettings = textStyle;
  modifyDirectives(style.directives);
  modifyDirectives(style.backDirectives);
  return style;
}

void TextPainter::clearTextStyle() {
  m_renderSettings = m_defaultRenderSettings;
}

void TextPainter::addFont(FontPtr const& font, String const& name) {
  m_fontTextureGroup.addFont(font, name);
}

void TextPainter::reloadFonts() {
  m_fontTextureGroup.clearFonts();
  m_fontTextureGroup.cleanup(0);
  auto assets = Root::singleton().assets();
  String defaultName = "hobo";
  auto defaultFont = loadFont("/hobo.ttf", defaultName);
  auto loadFontsByExtension = [&](String const& ext) {
    for (auto& fontPath : assets->scanExtension(ext)) {
      auto font = assets->font(fontPath);
      if (font == defaultFont)
        continue;

      auto name = AssetPath::filename(fontPath);
      name = name.substr(0, name.findLast("."));
      addFont(loadFont(fontPath, name), name);
    }
  };
  loadFontsByExtension("ttf");
  loadFontsByExtension("woff2");
  m_fontTextureGroup.addFont(defaultFont, defaultName, true);
}

void TextPainter::cleanup(int64_t timeout) {
  m_fontTextureGroup.cleanup(timeout);
}

void TextPainter::applyCommands(StringView unsplitCommands) {
  unsplitCommands.forEachSplitView(",", [&](StringView command, size_t, size_t) {
    try {
      if (command == "reset") {
        m_renderSettings = m_savedRenderSettings;
      } else if (command == "set") {
        m_savedRenderSettings = m_renderSettings;
      } else if (command.beginsWith("shadow")) {
        if (command.utf8Size() == 6)
          m_renderSettings.shadow = Color::Black.toRgba();
        else if (command[6] == '=')
          m_renderSettings.shadow = Color(command.substr(7)).toRgba();
      } else if (command == "noshadow") {
        m_renderSettings.shadow = Color::Clear.toRgba();
      } else if (command.beginsWith("font=")) {
        m_renderSettings.font = command.substr(5);
      } else if (command.beginsWith("directives=")) {
        setProcessingDirectives(command.substr(11));
      } else if (command.beginsWith("backdirectives=")) {
        setProcessingDirectives(command.substr(15), true);
      } else {
        // expects both #... sequences and plain old color names.
        Color c = Color(command);
        c.setAlphaF(c.alphaF() * ((float)m_savedRenderSettings.color[3]) / 255);
        m_renderSettings.color = c.toRgba();
      }
    } catch (JsonException&) {
    } catch (ColorException&) {
    }
  });
}

void TextPainter::modifyDirectives(Directives& directives) {
  if (directives) {
    directives.loadOperations();
    for (auto& entry : directives->entries) {
      if (auto border = entry.operation.ptr<BorderImageOperation>())
        border->includeTransparent = true;
    }
  }
}

RectF TextPainter::doRenderText(StringView s, TextPositioning const& position, bool reallyRender, unsigned* charLimit) {
  Vec2F pos = position.pos;
  if (s.empty())
    return RectF(pos, pos);

  List<StringView> lines = wrapTextViews(s, position.wrapWidth);

  TextStyle backup = m_savedRenderSettings = m_renderSettings;
  int height = (lines.size() - 1) * backup.lineSpacing * backup.fontSize + backup.fontSize;
  if (position.vAnchor == VerticalAnchor::BottomAnchor)
    pos[1] += (height - backup.fontSize);
  else if (position.vAnchor == VerticalAnchor::VMidAnchor)
    pos[1] += (height - backup.fontSize) / 2;

  RectF bounds = RectF::withSize(pos, Vec2F());
  for (auto& i : lines) {
    bounds.combine(doRenderLine(i, { pos, position.hAnchor, position.vAnchor }, reallyRender, charLimit));
    pos[1] -= m_renderSettings.fontSize * m_renderSettings.lineSpacing;

    if (charLimit && *charLimit == 0)
      break;
  }

  m_renderSettings = std::move(backup);

  return bounds;
}

RectF TextPainter::doRenderLine(StringView text, TextPositioning const& position, bool reallyRender, unsigned* charLimit) {
  if (m_reloadTracker->pullTriggered())
    reloadFonts();
  TextPositioning pos = position;


  if (pos.hAnchor == HorizontalAnchor::RightAnchor) {
    StringView trimmedString = charLimit ? text.substr(0, *charLimit) : text;
    pos.pos[0] -= stringWidth(trimmedString);
    pos.hAnchor = HorizontalAnchor::LeftAnchor;
  } else if (pos.hAnchor == HorizontalAnchor::HMidAnchor) {
    StringView trimmedString = charLimit ? text.substr(0, *charLimit) : text;
    pos.pos[0] -= floor((float)stringWidth(trimmedString) / 2);
    pos.hAnchor = HorizontalAnchor::LeftAnchor;
  }

  String escapeCode;
  RectF bounds = RectF::withSize(pos.pos, Vec2F());
  Text::TextCallback textCallback = [&](StringView text) {
    for (String::Char c : text) {
      if (charLimit) {
        if (*charLimit == 0)
          return false;
        else
          --*charLimit;
      }
      RectF glyphBounds = doRenderGlyph(c, pos, reallyRender);
      bounds.combine(glyphBounds);
      pos.pos[0] += glyphBounds.width();
    }
    return true;
  };

  Text::CommandsCallback commandsCallback = [&](StringView commands) {
    applyCommands(commands);
    return true;
  };

  m_fontTextureGroup.switchFont(m_renderSettings.font);
  Text::processText(text, textCallback, commandsCallback);

  return bounds;
}

RectF TextPainter::doRenderGlyph(String::Char c, TextPositioning const& position, bool reallyRender) {
  if (c == '\n' || c == '\v' || c == '\r')
    return RectF();
  m_fontTextureGroup.switchFont(m_renderSettings.font);

  int width = glyphWidth(c);
  // Offset left by width if right anchored.
  float hOffset = 0;
  if (position.hAnchor == HorizontalAnchor::RightAnchor)
    hOffset = -width;
  else if (position.hAnchor == HorizontalAnchor::HMidAnchor)
    hOffset = -floor((float)width / 2);

  float vOffset = 0;
  if (position.vAnchor == VerticalAnchor::VMidAnchor)
    vOffset = -floor((float)m_renderSettings.fontSize / 2);
  else if (position.vAnchor == VerticalAnchor::TopAnchor)
    vOffset = -(float)m_renderSettings.fontSize;

  Directives* directives = m_renderSettings.directives ? &m_renderSettings.directives : nullptr;

  Vec2F pos = position.pos + Vec2F(hOffset, vOffset);
  if (reallyRender) {
    bool hasShadow = m_renderSettings.shadow[3] > 0;
    bool hasBackDirectives = m_renderSettings.backDirectives;
    if (hasShadow) {
      //Kae: unlike vanilla we draw only one shadow glyph instead of two, so i'm tweaking the alpha here
      Vec4B shadow = m_renderSettings.shadow;
      uint8_t alphaU = m_renderSettings.color[3] * byteToFloat(shadow[3]);
      if (alphaU != 255) {
        float alpha = byteToFloat(alphaU);
        shadow[3] = floatToByte(alpha * (1.5f - 0.5f * alpha));
      }
      else
        shadow[3] = alphaU;

      Directives const* shadowDirectives = hasBackDirectives ? &m_renderSettings.backDirectives : directives;
      renderGlyph(c, pos + Vec2F(0, -2), m_shadowPrimitives, m_renderSettings.fontSize, 1, shadow, shadowDirectives);
    }
    if (hasBackDirectives)
      renderGlyph(c, pos, m_backPrimitives, m_renderSettings.fontSize, 1, m_renderSettings.color, &m_renderSettings.backDirectives);

    auto& output = (hasShadow || hasBackDirectives) ? m_frontPrimitives : m_renderer->immediatePrimitives(); 
    renderGlyph(c, pos, output, m_renderSettings.fontSize, 1, m_renderSettings.color, directives);
  }

  return RectF::withSize(pos, {(float)width, (int)m_renderSettings.fontSize});
}

void TextPainter::renderPrimitives() {
  auto& destination = m_renderer->immediatePrimitives();
  std::move(std::begin(m_shadowPrimitives), std::end(m_shadowPrimitives), std::back_inserter(destination));
  m_shadowPrimitives.clear();
  std::move(std::begin(m_backPrimitives), std::end(m_backPrimitives), std::back_inserter(destination));
  m_backPrimitives.clear();
  std::move(std::begin(m_frontPrimitives), std::end(m_frontPrimitives), std::back_inserter(destination));
  m_frontPrimitives.clear();
}

void TextPainter::renderGlyph(String::Char c, Vec2F const& screenPos, List<RenderPrimitive>& out, unsigned fontSize,
    float scale, Vec4B const& color, Directives const* processingDirectives) {
  if (!fontSize)
    return;

  const FontTextureGroup::GlyphTexture& glyphTexture = m_fontTextureGroup.glyphTexture(c, fontSize, processingDirectives);
  Vec2F offset = glyphTexture.offset * scale;
  out.emplace_back(std::in_place_type_t<RenderQuad>(), glyphTexture.texture, Vec2F::round(screenPos + offset), scale, color, 0.0f);
}

FontPtr TextPainter::loadFont(String const& fontPath, Maybe<String> fontName) {
  if (!fontName) {
    auto name = AssetPath::filename(fontPath);
    fontName.emplace(name.substr(0, name.findLast(".")));
  }

  auto assets = Root::singleton().assets();

  auto font = assets->font(fontPath)->clone();
  if (auto fontConfig = assets->json("/interface.config:font").opt(*fontName)) {
    font->setAlphaThreshold(fontConfig->getUInt("alphaThreshold", 0));
  }
  return font;
}
}
