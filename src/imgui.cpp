// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "precompiled.h"

#include "imgui.h"

namespace fpl {
namespace gui {

enum Alignment {
  ALIGN_TOPLEFT,
  ALIGN_CENTER,
  ALIGN_BOTTOMRIGHT,
};

bool IsVertical(Layout layout) {
  return layout >= LAYOUT_VERTICAL_LEFT;
}

Alignment GetAlignment(Layout layout) {
  return static_cast<Alignment>(layout - (IsVertical(layout)
             ? LAYOUT_VERTICAL_LEFT - LAYOUT_HORIZONTAL_TOP
             : 0));
}

// This holds the transient state of a group while its layout is being
// calculated / rendered.
class Group {
 public:
  Group(bool _vertical, Alignment _align, int _spacing, size_t _element_idx)
    : vertical_(_vertical), align_(_align), spacing_(_spacing),
      size_(mathfu::kZeros2i), position_(mathfu::kZeros2i),
      element_idx_(_element_idx) {}

  // Extend this group with the size of a new element, and possibly spacing
  // if it wasn't the first element.
  void Extend(const vec2i &extension) {
    size_ = vertical_
      ? vec2i(std::max(size_.x(), extension.x()),
              size_.y() + extension.y() + (size_.y() ? spacing_ : 0))
      : vec2i(size_.x() + extension.x() + (size_.x() ? spacing_ : 0),
              std::max(size_.y(), extension.y()));
  }

  bool vertical_;
  Alignment align_;
  int spacing_;
  vec2i size_;
  vec2i position_;
  size_t element_idx_;
};

// This holds transient state used while a GUI is being laid out / rendered.
// It is intentionally hidden from the interface.
// It is implemented as a singleton that the GUI element functions can access.

class InternalState;
InternalState *state = nullptr;

class InternalState : Group {
 public:
  struct Element {
    Element(const char *_id, const vec2i &_size)
      : id(_id), size(_size) {}

    const char *id;
    vec2i size;
  };

  InternalState(MaterialManager &matman, InputSystem &input)
      : Group(true, ALIGN_TOPLEFT, 0, 0),
        layout_pass_(true), canvas_size_(matman.renderer().window_size()),
        virtual_resolution_(IMGUI_DEFAULT_VIRTUAL_RESOLUTION),
        matman_(matman),
        input_(input) {
    SetScale();

    // If this assert hits, you likely are trying to created nested GUIs.
    assert(!state);

    state = this;
  }

  ~InternalState() {
    state = nullptr;
  }

  // Initialize the scaling factor for the virtual resolution.
  void SetScale() {
    auto scale = vec2(matman_.renderer().window_size()) / virtual_resolution_;
    pixel_scale_ = std::min(scale.x(), scale.y());
  }

  // Compute a space offset for a particular alignment for just the x or y
  // dimension.
  static vec2i AlignDimension(Alignment align, int dim, const vec2i &space) {
    vec2i dest(0, 0);
    switch (align) {
      case ALIGN_TOPLEFT:
        break;
      case ALIGN_CENTER:
        dest[dim] += space[dim] / 2;
        break;
      case ALIGN_BOTTOMRIGHT:
        dest[dim] += space[dim];
        break;
    }
    return dest;
  }

  // Determines placement for the UI as a whole inside the available space
  // (screen).
  void PositionUI(const vec2i &canvas_size, float virtual_resolution,
                  Alignment horizontal, Alignment vertical) {
    if (layout_pass_) {
      canvas_size_ = canvas_size;
      virtual_resolution_ = virtual_resolution;
      SetScale();
    } else {
      auto space = canvas_size_ - size_;
      position_ += AlignDimension(horizontal, 0, space) +
                   AlignDimension(vertical,   1, space);
    }
  }

  // Switch from the layout pass to the render/event pass.
  void StartRenderPass() {
    // If you hit this assert, you are missing an EndGroup().
    assert(!group_stack_.size());

    size_ = elements_[0].size;

    layout_pass_ = false;
    element_it_ = elements_.begin();
  }

  // (render pass): retrieve the next corresponding cached element we
  // created in the layout pass. This is slightly more tricky than a straight
  // lookup because event handlers may insert/remove elements.
  const Element *NextElement(const char *id) {
    auto backup = element_it_;
    while (element_it_ != elements_.end()) {
      // This loop usually returns on the first iteration, the only time it
      // doesn't is if an event handler caused an element to removed.
      auto &element = *element_it_;
      ++element_it_;
      if (!strcmp(element.id, id)) return &element;
    }
    // Didn't find this id at all, which means an event handler just caused
    // this element to be added, so we skip it.
    element_it_ = backup;
    return nullptr;
  }

  // (layout pass): create a new element.
  void NewElement(const char *id, const vec2i &size) {
    elements_.push_back(Element(id, size));
  }

  // (render pass): move the group's current position past an element of
  // the given size.
  void Advance(const vec2i &size) {
    position_ += vertical_
      ? vec2i(0, size.y() + spacing_)
      : vec2i(size.x() + spacing_, 0);
  }

  // (render pass): return the position of the current element, as a function
  // of the group's current position and the alignment.
  vec2i Position(const Element &element) {
    return position_ +
           AlignDimension(align_, vertical_ ? 0 : 1,
                          size_ - element.size);
  }

  // An image element.
  void Image(const char *texture_name, float ysize) {
    auto tex = matman_.FindTexture(texture_name);
    assert(tex);  // You need to have called LoadTexture before.
    if (layout_pass_) {
      auto virtual_image_size =
          vec2(tex->size().x() * ysize / tex->size().y(), ysize);
      // Map the size to real screen pixels, rounding to the nearest int
      // for pixel-aligned rendering.
      auto size = vec2i(virtual_image_size * pixel_scale_ + 0.5f);
      NewElement(texture_name, size);
      Extend(size);
    } else {
      auto element = NextElement(texture_name);
      if (element) {
        auto position = Position(*element);
        tex->Set(0);
        Mesh::RenderAAQuadAlongX(vec3(vec2(position), 0),
                                 vec3(vec2(position + element->size), 0));
        Advance(element->size);
      }
    }
  }

  // An element that has sub-elements. Tracks its state in an instance of
  // Layout, that is pushed/popped from the stack as needed.
  void StartGroup(bool vertical, Alignment align, int spacing) {
    auto group_id = "__group__";
    Group layout(vertical, align, spacing, elements_.size());
    group_stack_.push_back(*this);
    if (layout_pass_) {
      NewElement(group_id, mathfu::kZeros2i);
    } else {
      auto element = NextElement(group_id);
      if (element) {
        layout.position_ = Position(*element);
        layout.size_ = element->size;
      }
    }
    *static_cast<Group *>(this) = layout;
  }

  // Clean up the Group element started by StartGroup()
  void EndGroup() {
    // If you hit this assert, you have one too many EndGroup().
    assert(group_stack_.size());

    auto size = size_;
    auto element_idx = element_idx_;
    *static_cast<Group *>(this) = group_stack_.back();
    group_stack_.pop_back();
    if (layout_pass_) {
      // Contribute the size of this group to its parent.
      Extend(size);
      // Set the size of this group as the size of the element tracking it.
      elements_[element_idx].size = size;
    } else {
      Advance(size);
    }
  }

  bool layout_pass_;
  std::vector<Element> elements_;
  std::vector<Element>::const_iterator element_it_;
  std::vector<Group> group_stack_;
  vec2i canvas_size_;
  float virtual_resolution_;
  float pixel_scale_;
  MaterialManager &matman_;
  InputSystem &input_;
};

void Run(MaterialManager &matman, InputSystem &input,
         const std::function<void ()> &gui_definition)
{

  // Create our new temporary state.
  InternalState internal_state(matman, input);

  // Run two passes, one for layout, one for rendering.
  // First pass:
  gui_definition();

  // Second pass:
  internal_state.StartRenderPass();

  // Set up an ortho camera for all 2D elements, with (0, 0) in the top left,
  // and the bottom right the windows size in pixels.
  auto &renderer = matman.renderer();
  auto res = renderer.window_size();
  auto ortho_mat = mathfu::OrthoHelper<float>(
      0.0f, static_cast<float>(res.x()), static_cast<float>(res.y()), 0.0f,
      -1.0f, 1.0f);
  renderer.model_view_projection() = ortho_mat;

  // TODO: must be user configurable.
  //renderer.SetBlendMode(kBlendModeOff);
  renderer.color() = mathfu::kOnes4f;
  matman.LoadShader("shaders/textured")->Set(renderer);

  gui_definition();
}

InternalState *Gui() { assert(state); return state; }

void Image(const char *texture_name, float size)
{
  Gui()->Image(texture_name, size);
}

void StartGroup(Layout layout, int spacing) {
  Gui()->StartGroup(IsVertical(layout), GetAlignment(layout), spacing);
}

void EndGroup() {
  Gui()->EndGroup();
}

void PositionUI(const vec2i &canvas_size, float virtual_resolution,
                Layout horizontal, Layout vertical) {
  Gui()->PositionUI(canvas_size, virtual_resolution, GetAlignment(horizontal),
                    GetAlignment(vertical));
}

void TestGUI(MaterialManager &matman, InputSystem &input) {
  Run(matman, input, [&matman]() {
    PositionUI(matman.renderer().window_size(), 1000, LAYOUT_HORIZONTAL_CENTER,
               LAYOUT_VERTICAL_RIGHT);
    StartGroup(LAYOUT_HORIZONTAL_TOP);
      StartGroup(LAYOUT_VERTICAL_LEFT, 20);
        Image("textures/text_about.webp", 50);
        Image("textures/text_about.webp", 40);
        Image("textures/text_about.webp", 30);
      EndGroup();
      StartGroup(LAYOUT_VERTICAL_CENTER, 40);
        Image("textures/text_about.webp", 50);
        Image("textures/text_about.webp", 40);
        Image("textures/text_about.webp", 30);
      EndGroup();
      StartGroup(LAYOUT_VERTICAL_RIGHT, 0);
        Image("textures/text_about.webp", 50);
        Image("textures/text_about.webp", 40);
        Image("textures/text_about.webp", 30);
      EndGroup();
    EndGroup();
  });
}

}  // namespace gui
}  // namespace fpl
