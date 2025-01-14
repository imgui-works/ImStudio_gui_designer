// dear imgui: Renderer + Platform Backend for Allegro 5
// (Info: Allegro 5 is a cross-platform general purpose library for handling windows, inputs, graphics, etc.)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'ALLEGRO_BITMAP*' as ImTextureID. Read the FAQ about ImTextureID!
//  [X] Platform: Clipboard support (from Allegro 5.1.12)
//  [X] Platform: Mouse cursor shape and visibility. Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
// Issues:
//  [ ] Renderer: The renderer is suboptimal as we need to unindex our buffers and convert vertices manually.
//  [ ] Platform: Missing gamepad support.

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2021-08-17: Calling io.AddFocusEvent() on ALLEGRO_EVENT_DISPLAY_SWITCH_OUT/ALLEGRO_EVENT_DISPLAY_SWITCH_IN events.
//  2021-06-29: Reorganized backend to pull data from a single structure to facilitate usage with multiple-contexts (all g_XXXX access changed to bd->XXXX).
//  2021-05-19: Renderer: Replaced direct access to ImDrawCmd::TextureId with a call to ImDrawCmd::GetTexID(). (will become a requirement)
//  2021-02-18: Change blending equation to preserve alpha in output buffer.
//  2020-08-10: Inputs: Fixed horizontal mouse wheel direction.
//  2019-12-05: Inputs: Added support for ImGuiMouseCursor_NotAllowed mouse cursor.
//  2019-07-21: Inputs: Added mapping for ImGuiKey_KeyPadEnter.
//  2019-05-11: Inputs: Don't filter character value from ALLEGRO_EVENT_KEY_CHAR before calling AddInputCharacter().
//  2019-04-30: Renderer: Added support for special ImDrawCallback_ResetRenderState callback to reset render state.
//  2018-11-30: Platform: Added touchscreen support.
//  2018-11-30: Misc: Setting up io.BackendPlatformName/io.BackendRendererName so they can be displayed in the About Window.
//  2018-06-13: Platform: Added clipboard support (from Allegro 5.1.12).
//  2018-06-13: Renderer: Use draw_data->DisplayPos and draw_data->DisplaySize to setup projection matrix and clipping rectangle.
//  2018-06-13: Renderer: Backup/restore transform and clipping rectangle.
//  2018-06-11: Misc: Setup io.BackendFlags ImGuiBackendFlags_HasMouseCursors flag + honor ImGuiConfigFlags_NoMouseCursorChange flag.
//  2018-04-18: Misc: Renamed file from imgui_impl_a5.cpp to imgui_impl_allegro5.cpp.
//  2018-04-18: Misc: Added support for 32-bit vertex indices to avoid conversion at runtime. Added imconfig_allegro5.h to enforce 32-bit indices when included from imgui.h.
//  2018-02-16: Misc: Obsoleted the io.RenderDrawListsFn callback and exposed ImGui_ImplAllegro5_RenderDrawData() in the .h file so you can call it yourself.
//  2018-02-06: Misc: Removed call to ImGui::Shutdown() which is not available from 1.60 WIP, user needs to call CreateContext/DestroyContext themselves.
//  2018-02-06: Inputs: Added mapping for ImGuiKey_Space.

#include <stdint.h>     // uint64_t
#include <cstring>      // memcpy
#include "imgui.h"
#include "imgui_impl_allegro5.h"

// Allegro
#include <allegro5/allegro.h>
#include <allegro5/allegro_primitives.h>
#ifdef _WIN32
#include <allegro5/allegro_windows.h>
#endif
#define ALLEGRO_HAS_CLIPBOARD   (ALLEGRO_VERSION_INT >= ((5 << 24) | (1 << 16) | (12 << 8)))    // Clipboard only supported from Allegro 5.1.12

// Visual Studio warnings
#ifdef _MSC_VER
#pragma warning (disable: 4127) // condition expression is constant
#endif

// Allegro Data
struct ImGui_ImplAllegro5_Data
{
    ALLEGRO_DISPLAY*            Display;
    ALLEGRO_BITMAP*             Texture;
    double                      Time;
    ALLEGRO_MOUSE_CURSOR*       MouseCursorInvisible;
    ALLEGRO_VERTEX_DECL*        VertexDecl;
    char*                       ClipboardTextData;

    ImGui_ImplAllegro5_Data()   { memset(this, 0, sizeof(*this)); }
};

// Backend data stored in io.BackendPlatformUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
// FIXME: multi-context support is not well tested and probably dysfunctional in this backend.
static ImGui_ImplAllegro5_Data* ImGui_ImplAllegro5_GetBackendData()     { return ImGui::GetCurrentContext() ? (ImGui_ImplAllegro5_Data*)ImGui::GetIO().BackendPlatformUserData : NULL; }

struct ImDrawVertAllegro
{
    ImVec2 pos;
    ImVec2 uv;
    ALLEGRO_COLOR col;
};

static void ImGui_ImplAllegro5_SetupRenderState(ImDrawData* draw_data)
{
    // Setup blending
    al_set_separate_blender(ALLEGRO_ADD, ALLEGRO_ALPHA, ALLEGRO_INVERSE_ALPHA, ALLEGRO_ADD, ALLEGRO_ONE, ALLEGRO_INVERSE_ALPHA);

    // Setup orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right).
    {
        float L = draw_data->DisplayPos.x;
        float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float T = draw_data->DisplayPos.y;
        float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        ALLEGRO_TRANSFORM transform;
        al_identity_transform(&transform);
        al_use_transform(&transform);
        al_orthographic_transform(&transform, L, T, 1.0f, R, B, -1.0f);
        al_use_projection_transform(&transform);
    }
}

// Render function.
void ImGui_ImplAllegro5_RenderDrawData(ImDrawData* draw_data)
{
    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    // Backup Allegro state that will be modified
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    ALLEGRO_TRANSFORM last_transform = *al_get_current_transform();
    ALLEGRO_TRANSFORM last_projection_transform = *al_get_current_projection_transform();
    int last_clip_x, last_clip_y, last_clip_w, last_clip_h;
    al_get_clipping_rectangle(&last_clip_x, &last_clip_y, &last_clip_w, &last_clip_h);
    int last_blender_op, last_blender_src, last_blender_dst;
    al_get_blender(&last_blender_op, &last_blender_src, &last_blender_dst);

    // Setup desired render state
    ImGui_ImplAllegro5_SetupRenderState(draw_data);

    // Render command lists
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];

        // Allegro's implementation of al_draw_indexed_prim() for DX9 is completely broken. Unindex our buffers ourselves.
        // FIXME-OPT: Unfortunately Allegro doesn't support 32-bit packed colors so we have to convert them to 4 float as well..
        static ImVector<ImDrawVertAllegro> vertices;
        vertices.resize(cmd_list->IdxBuffer.Size);
        for (int i = 0; i < cmd_list->IdxBuffer.Size; i++)
        {
            const ImDrawVert* src_v = &cmd_list->VtxBuffer[cmd_list->IdxBuffer[i]];
            ImDrawVertAllegro* dst_v = &vertices[i];
            dst_v->pos = src_v->pos;
            dst_v->uv = src_v->uv;
            unsigned char* c = (unsigned char*)&src_v->col;
            dst_v->col = al_map_rgba(c[0], c[1], c[2], c[3]);
        }

        const int* indices = NULL;
        if (sizeof(ImDrawIdx) == 2)
        {
            // FIXME-OPT: Unfortunately Allegro doesn't support 16-bit indices.. You can '#define ImDrawIdx int' in imconfig.h to request Dear ImGui to output 32-bit indices.
            // Otherwise, we convert them from 16-bit to 32-bit at runtime here, which works perfectly but is a little wasteful.
            static ImVector<int> indices_converted;
            indices_converted.resize(cmd_list->IdxBuffer.Size);
            for (int i = 0; i < cmd_list->IdxBuffer.Size; ++i)
                indices_converted[i] = (int)cmd_list->IdxBuffer.Data[i];
            indices = indices_converted.Data;
        }
        else if (sizeof(ImDrawIdx) == 4)
        {
            indices = (const int*)cmd_list->IdxBuffer.Data;
        }

        // Render command lists
        int idx_offset = 0;
        ImVec2 clip_off = draw_data->DisplayPos;
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback)
            {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    ImGui_ImplAllegro5_SetupRenderState(draw_data);
                else
                    pcmd->UserCallback(cmd_list, pcmd);
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min(pcmd->ClipRect.x - clip_off.x, pcmd->ClipRect.y - clip_off.y);
                ImVec2 clip_max(pcmd->ClipRect.z - clip_off.x, pcmd->ClipRect.w - clip_off.y);
                if (clip_max.x < clip_min.x || clip_max.y < clip_min.y)
                    continue;

                // Apply scissor/clipping rectangle, Draw
                ALLEGRO_BITMAP* texture = (ALLEGRO_BITMAP*)pcmd->GetTexID();
                al_set_clipping_rectangle(clip_min.x, clip_min.y, clip_max.x - clip_min.x, clip_max.y - clip_min.y);
                al_draw_prim(&vertices[0], bd->VertexDecl, texture, idx_offset, idx_offset + pcmd->ElemCount, ALLEGRO_PRIM_TRIANGLE_LIST);
            }
            idx_offset += pcmd->ElemCount;
        }
    }

    // Restore modified Allegro state
    al_set_blender(last_blender_op, last_blender_src, last_blender_dst);
    al_set_clipping_rectangle(last_clip_x, last_clip_y, last_clip_w, last_clip_h);
    al_use_transform(&last_transform);
    al_use_projection_transform(&last_projection_transform);
}

bool ImGui_ImplAllegro5_CreateDeviceObjects()
{
    // Build texture atlas
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    // Create texture
    int flags = al_get_new_bitmap_flags();
    int fmt = al_get_new_bitmap_format();
    al_set_new_bitmap_flags(ALLEGRO_MEMORY_BITMAP | ALLEGRO_MIN_LINEAR | ALLEGRO_MAG_LINEAR);
    al_set_new_bitmap_format(ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE);
    ALLEGRO_BITMAP* img = al_create_bitmap(width, height);
    al_set_new_bitmap_flags(flags);
    al_set_new_bitmap_format(fmt);
    if (!img)
        return false;

    ALLEGRO_LOCKED_REGION* locked_img = al_lock_bitmap(img, al_get_bitmap_format(img), ALLEGRO_LOCK_WRITEONLY);
    if (!locked_img)
    {
        al_destroy_bitmap(img);
        return false;
    }
    memcpy(locked_img->data, pixels, sizeof(int) * width * height);
    al_unlock_bitmap(img);

    // Convert software texture to hardware texture.
    ALLEGRO_BITMAP* cloned_img = al_clone_bitmap(img);
    al_destroy_bitmap(img);
    if (!cloned_img)
        return false;

    // Store our identifier
    io.Fonts->SetTexID((void*)cloned_img);
    bd->Texture = cloned_img;

    // Create an invisible mouse cursor
    // Because al_hide_mouse_cursor() seems to mess up with the actual inputs..
    ALLEGRO_BITMAP* mouse_cursor = al_create_bitmap(8, 8);
    bd->MouseCursorInvisible = al_create_mouse_cursor(mouse_cursor, 0, 0);
    al_destroy_bitmap(mouse_cursor);

    return true;
}

void ImGui_ImplAllegro5_InvalidateDeviceObjects()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    if (bd->Texture)
    {
        io.Fonts->SetTexID(NULL);
        al_destroy_bitmap(bd->Texture);
        bd->Texture = NULL;
    }
    if (bd->MouseCursorInvisible)
    {
        al_destroy_mouse_cursor(bd->MouseCursorInvisible);
        bd->MouseCursorInvisible = NULL;
    }
}

#if ALLEGRO_HAS_CLIPBOARD
static const char* ImGui_ImplAllegro5_GetClipboardText(void*)
{
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    if (bd->ClipboardTextData)
        al_free(bd->ClipboardTextData);
    bd->ClipboardTextData = al_get_clipboard_text(bd->Display);
    return bd->ClipboardTextData;
}

static void ImGui_ImplAllegro5_SetClipboardText(void*, const char* text)
{
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    al_set_clipboard_text(bd->Display, text);
}
#endif

bool ImGui_ImplAllegro5_Init(ALLEGRO_DISPLAY* display)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == NULL && "Already initialized a platform backend!");

    // Setup backend capabilities flags
    ImGui_ImplAllegro5_Data* bd = IM_NEW(ImGui_ImplAllegro5_Data)();
    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = io.BackendRendererName = "imgui_impl_allegro5";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;       // We can honor GetMouseCursor() values (optional)

    bd->Display = display;

    // Create custom vertex declaration.
    // Unfortunately Allegro doesn't support 32-bit packed colors so we have to convert them to 4 floats.
    // We still use a custom declaration to use 'ALLEGRO_PRIM_TEX_COORD' instead of 'ALLEGRO_PRIM_TEX_COORD_PIXEL' else we can't do a reliable conversion.
    ALLEGRO_VERTEX_ELEMENT elems[] =
    {
        { ALLEGRO_PRIM_POSITION, ALLEGRO_PRIM_FLOAT_2, IM_OFFSETOF(ImDrawVertAllegro, pos) },
        { ALLEGRO_PRIM_TEX_COORD, ALLEGRO_PRIM_FLOAT_2, IM_OFFSETOF(ImDrawVertAllegro, uv) },
        { ALLEGRO_PRIM_COLOR_ATTR, 0, IM_OFFSETOF(ImDrawVertAllegro, col) },
        { 0, 0, 0 }
    };
    bd->VertexDecl = al_create_vertex_decl(elems, sizeof(ImDrawVertAllegro));

    io.KeyMap[ImGuiKey_Tab] = ALLEGRO_KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = ALLEGRO_KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = ALLEGRO_KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = ALLEGRO_KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow] = ALLEGRO_KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = ALLEGRO_KEY_PGUP;
    io.KeyMap[ImGuiKey_PageDown] = ALLEGRO_KEY_PGDN;
    io.KeyMap[ImGuiKey_Home] = ALLEGRO_KEY_HOME;
    io.KeyMap[ImGuiKey_End] = ALLEGRO_KEY_END;
    io.KeyMap[ImGuiKey_Insert] = ALLEGRO_KEY_INSERT;
    io.KeyMap[ImGuiKey_Delete] = ALLEGRO_KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = ALLEGRO_KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Space] = ALLEGRO_KEY_SPACE;
    io.KeyMap[ImGuiKey_Enter] = ALLEGRO_KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape] = ALLEGRO_KEY_ESCAPE;
    io.KeyMap[ImGuiKey_Apostrophe] = ALLEGRO_KEY_QUOTE; // '
    io.KeyMap[ImGuiKey_Comma] = ALLEGRO_KEY_COMMA; // ,
    io.KeyMap[ImGuiKey_Minus] = ALLEGRO_KEY_MINUS; // -
    io.KeyMap[ImGuiKey_Period] = ALLEGRO_KEY_FULLSTOP; // .
    io.KeyMap[ImGuiKey_Slash] = ALLEGRO_KEY_SLASH; // /
    io.KeyMap[ImGuiKey_Semicolon] = ALLEGRO_KEY_SEMICOLON; // ;
    io.KeyMap[ImGuiKey_Equal] = ALLEGRO_KEY_EQUALS; // =
    io.KeyMap[ImGuiKey_LeftBracket] = ALLEGRO_KEY_OPENBRACE; // [
    io.KeyMap[ImGuiKey_Backslash] = ALLEGRO_KEY_BACKSLASH; // \ (this text inhibit multiline comment caused by backlash)
    io.KeyMap[ImGuiKey_RightBracket] = ALLEGRO_KEY_CLOSEBRACE; // ]
    io.KeyMap[ImGuiKey_GraveAccent] = ALLEGRO_KEY_TILDE; // `
    io.KeyMap[ImGuiKey_CapsLock] = ALLEGRO_KEY_CAPSLOCK;
    io.KeyMap[ImGuiKey_ScrollLock] = ALLEGRO_KEY_SCROLLLOCK;
    io.KeyMap[ImGuiKey_NumLock] = ALLEGRO_KEY_NUMLOCK;
    io.KeyMap[ImGuiKey_PrintScreen] = ALLEGRO_KEY_PRINTSCREEN;
    io.KeyMap[ImGuiKey_Pause] = ALLEGRO_KEY_PAUSE;
    io.KeyMap[ImGuiKey_KeyPad0] = ALLEGRO_KEY_PAD_0;
    io.KeyMap[ImGuiKey_KeyPad1] = ALLEGRO_KEY_PAD_1;
    io.KeyMap[ImGuiKey_KeyPad2] = ALLEGRO_KEY_PAD_2;
    io.KeyMap[ImGuiKey_KeyPad3] = ALLEGRO_KEY_PAD_3;
    io.KeyMap[ImGuiKey_KeyPad4] = ALLEGRO_KEY_PAD_4;
    io.KeyMap[ImGuiKey_KeyPad5] = ALLEGRO_KEY_PAD_5;
    io.KeyMap[ImGuiKey_KeyPad6] = ALLEGRO_KEY_PAD_6;
    io.KeyMap[ImGuiKey_KeyPad7] = ALLEGRO_KEY_PAD_7;
    io.KeyMap[ImGuiKey_KeyPad8] = ALLEGRO_KEY_PAD_8;
    io.KeyMap[ImGuiKey_KeyPad9] = ALLEGRO_KEY_PAD_9;
    io.KeyMap[ImGuiKey_KeyPadDecimal] = ALLEGRO_KEY_PAD_DELETE;
    io.KeyMap[ImGuiKey_KeyPadDivide] = ALLEGRO_KEY_PAD_SLASH;
    io.KeyMap[ImGuiKey_KeyPadMultiply] = ALLEGRO_KEY_PAD_ASTERISK;
    io.KeyMap[ImGuiKey_KeyPadSubtract] = ALLEGRO_KEY_PAD_MINUS;
    io.KeyMap[ImGuiKey_KeyPadAdd] = ALLEGRO_KEY_PAD_PLUS;
    io.KeyMap[ImGuiKey_KeyPadEnter] = ALLEGRO_KEY_PAD_ENTER;
    io.KeyMap[ImGuiKey_KeyPadEqual] = ALLEGRO_KEY_PAD_EQUALS;
    io.KeyMap[ImGuiKey_LeftShift] = ALLEGRO_KEY_LSHIFT;
    io.KeyMap[ImGuiKey_LeftControl] = ALLEGRO_KEY_LCTRL;
    io.KeyMap[ImGuiKey_LeftAlt] = ALLEGRO_KEY_ALT;
    io.KeyMap[ImGuiKey_LeftSuper] = ALLEGRO_KEY_LWIN;
    io.KeyMap[ImGuiKey_RightShift] = ALLEGRO_KEY_RSHIFT;
    io.KeyMap[ImGuiKey_RightControl] = ALLEGRO_KEY_RCTRL;
    io.KeyMap[ImGuiKey_RightAlt] = ALLEGRO_KEY_ALTGR;
    io.KeyMap[ImGuiKey_RightSuper] = ALLEGRO_KEY_RWIN;
    io.KeyMap[ImGuiKey_Menu] = ALLEGRO_KEY_MENU;
    io.KeyMap[ImGuiKey_0] = ALLEGRO_KEY_0;
    io.KeyMap[ImGuiKey_1] = ALLEGRO_KEY_1;
    io.KeyMap[ImGuiKey_2] = ALLEGRO_KEY_2;
    io.KeyMap[ImGuiKey_3] = ALLEGRO_KEY_3;
    io.KeyMap[ImGuiKey_4] = ALLEGRO_KEY_4;
    io.KeyMap[ImGuiKey_5] = ALLEGRO_KEY_5;
    io.KeyMap[ImGuiKey_6] = ALLEGRO_KEY_6;
    io.KeyMap[ImGuiKey_7] = ALLEGRO_KEY_7;
    io.KeyMap[ImGuiKey_8] = ALLEGRO_KEY_8;
    io.KeyMap[ImGuiKey_9] = ALLEGRO_KEY_9;
    io.KeyMap[ImGuiKey_A] = ALLEGRO_KEY_A;
    io.KeyMap[ImGuiKey_B] = ALLEGRO_KEY_B;
    io.KeyMap[ImGuiKey_C] = ALLEGRO_KEY_C;
    io.KeyMap[ImGuiKey_D] = ALLEGRO_KEY_D;
    io.KeyMap[ImGuiKey_E] = ALLEGRO_KEY_E;
    io.KeyMap[ImGuiKey_F] = ALLEGRO_KEY_F;
    io.KeyMap[ImGuiKey_G] = ALLEGRO_KEY_G;
    io.KeyMap[ImGuiKey_H] = ALLEGRO_KEY_H;
    io.KeyMap[ImGuiKey_I] = ALLEGRO_KEY_I;
    io.KeyMap[ImGuiKey_J] = ALLEGRO_KEY_J;
    io.KeyMap[ImGuiKey_K] = ALLEGRO_KEY_K;
    io.KeyMap[ImGuiKey_L] = ALLEGRO_KEY_L;
    io.KeyMap[ImGuiKey_M] = ALLEGRO_KEY_M;
    io.KeyMap[ImGuiKey_N] = ALLEGRO_KEY_N;
    io.KeyMap[ImGuiKey_O] = ALLEGRO_KEY_O;
    io.KeyMap[ImGuiKey_P] = ALLEGRO_KEY_P;
    io.KeyMap[ImGuiKey_Q] = ALLEGRO_KEY_Q;
    io.KeyMap[ImGuiKey_R] = ALLEGRO_KEY_R;
    io.KeyMap[ImGuiKey_S] = ALLEGRO_KEY_S;
    io.KeyMap[ImGuiKey_T] = ALLEGRO_KEY_T;
    io.KeyMap[ImGuiKey_U] = ALLEGRO_KEY_U;
    io.KeyMap[ImGuiKey_V] = ALLEGRO_KEY_V;
    io.KeyMap[ImGuiKey_W] = ALLEGRO_KEY_W;
    io.KeyMap[ImGuiKey_X] = ALLEGRO_KEY_X;
    io.KeyMap[ImGuiKey_Y] = ALLEGRO_KEY_Y;
    io.KeyMap[ImGuiKey_Z] = ALLEGRO_KEY_Z;
    io.KeyMap[ImGuiKey_F1] = ALLEGRO_KEY_F1;
    io.KeyMap[ImGuiKey_F2] = ALLEGRO_KEY_F2;
    io.KeyMap[ImGuiKey_F3] = ALLEGRO_KEY_F3;
    io.KeyMap[ImGuiKey_F4] = ALLEGRO_KEY_F4;
    io.KeyMap[ImGuiKey_F5] = ALLEGRO_KEY_F5;
    io.KeyMap[ImGuiKey_F6] = ALLEGRO_KEY_F6;
    io.KeyMap[ImGuiKey_F7] = ALLEGRO_KEY_F7;
    io.KeyMap[ImGuiKey_F8] = ALLEGRO_KEY_F8;
    io.KeyMap[ImGuiKey_F9] = ALLEGRO_KEY_F9;
    io.KeyMap[ImGuiKey_F10] = ALLEGRO_KEY_F10;
    io.KeyMap[ImGuiKey_F11] = ALLEGRO_KEY_F11;
    io.KeyMap[ImGuiKey_F12] = ALLEGRO_KEY_F12;
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

#if ALLEGRO_HAS_CLIPBOARD
    io.SetClipboardTextFn = ImGui_ImplAllegro5_SetClipboardText;
    io.GetClipboardTextFn = ImGui_ImplAllegro5_GetClipboardText;
    io.ClipboardUserData = NULL;
#endif

    return true;
}

void ImGui_ImplAllegro5_Shutdown()
{
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    IM_ASSERT(bd != NULL && "No platform backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    ImGui_ImplAllegro5_InvalidateDeviceObjects();
    if (bd->VertexDecl)
        al_destroy_vertex_decl(bd->VertexDecl);
    if (bd->ClipboardTextData)
        al_free(bd->ClipboardTextData);

    io.BackendPlatformUserData = NULL;
    io.BackendPlatformName = io.BackendRendererName = NULL;
    IM_DELETE(bd);
}

// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
bool ImGui_ImplAllegro5_ProcessEvent(ALLEGRO_EVENT* ev)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();

    switch (ev->type)
    {
    case ALLEGRO_EVENT_MOUSE_AXES:
        if (ev->mouse.display == bd->Display)
        {
            io.MouseWheel += ev->mouse.dz;
            io.MouseWheelH -= ev->mouse.dw;
            io.MousePos = ImVec2(ev->mouse.x, ev->mouse.y);
        }
        return true;
    case ALLEGRO_EVENT_MOUSE_BUTTON_DOWN:
    case ALLEGRO_EVENT_MOUSE_BUTTON_UP:
        if (ev->mouse.display == bd->Display && ev->mouse.button <= 5)
            io.MouseDown[ev->mouse.button - 1] = (ev->type == ALLEGRO_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    case ALLEGRO_EVENT_TOUCH_MOVE:
        if (ev->touch.display == bd->Display)
            io.MousePos = ImVec2(ev->touch.x, ev->touch.y);
        return true;
    case ALLEGRO_EVENT_TOUCH_BEGIN:
    case ALLEGRO_EVENT_TOUCH_END:
    case ALLEGRO_EVENT_TOUCH_CANCEL:
        if (ev->touch.display == bd->Display && ev->touch.primary)
            io.MouseDown[0] = (ev->type == ALLEGRO_EVENT_TOUCH_BEGIN);
        return true;
    case ALLEGRO_EVENT_MOUSE_LEAVE_DISPLAY:
        if (ev->mouse.display == bd->Display)
            io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        return true;
    case ALLEGRO_EVENT_KEY_CHAR:
        if (ev->keyboard.display == bd->Display)
            if (ev->keyboard.unichar != 0)
                io.AddInputCharacter((unsigned int)ev->keyboard.unichar);
        return true;
    case ALLEGRO_EVENT_KEY_DOWN:
    case ALLEGRO_EVENT_KEY_UP:
        if (ev->keyboard.display == bd->Display)
            io.KeysDown[ev->keyboard.keycode] = (ev->type == ALLEGRO_EVENT_KEY_DOWN);
        return true;
    case ALLEGRO_EVENT_DISPLAY_SWITCH_OUT:
        if (ev->display.source == bd->Display)
            io.AddFocusEvent(false);
        return true;
    case ALLEGRO_EVENT_DISPLAY_SWITCH_IN:
        if (ev->display.source == bd->Display)
        {
            io.AddFocusEvent(true);
#if defined(ALLEGRO_UNSTABLE)
            al_clear_keyboard_state(bd->Display);
#endif
        }
        return true;
    }
    return false;
}

static void ImGui_ImplAllegro5_UpdateMouseCursor()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    if (io.MouseDrawCursor || imgui_cursor == ImGuiMouseCursor_None)
    {
        // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
        al_set_mouse_cursor(bd->Display, bd->MouseCursorInvisible);
    }
    else
    {
        ALLEGRO_SYSTEM_MOUSE_CURSOR cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_DEFAULT;
        switch (imgui_cursor)
        {
        case ImGuiMouseCursor_TextInput:    cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_EDIT; break;
        case ImGuiMouseCursor_ResizeAll:    cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_MOVE; break;
        case ImGuiMouseCursor_ResizeNS:     cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_RESIZE_N; break;
        case ImGuiMouseCursor_ResizeEW:     cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_RESIZE_E; break;
        case ImGuiMouseCursor_ResizeNESW:   cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_RESIZE_NE; break;
        case ImGuiMouseCursor_ResizeNWSE:   cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_RESIZE_NW; break;
        case ImGuiMouseCursor_NotAllowed:   cursor_id = ALLEGRO_SYSTEM_MOUSE_CURSOR_UNAVAILABLE; break;
        }
        al_set_system_mouse_cursor(bd->Display, cursor_id);
    }
}

void ImGui_ImplAllegro5_NewFrame()
{
    ImGui_ImplAllegro5_Data* bd = ImGui_ImplAllegro5_GetBackendData();
    IM_ASSERT(bd != NULL && "Did you call ImGui_ImplAllegro5_Init()?");

    if (!bd->Texture)
        ImGui_ImplAllegro5_CreateDeviceObjects();

    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    int w, h;
    w = al_get_display_width(bd->Display);
    h = al_get_display_height(bd->Display);
    io.DisplaySize = ImVec2((float)w, (float)h);

    // Setup time step
    double current_time = al_get_time();
    io.DeltaTime = bd->Time > 0.0 ? (float)(current_time - bd->Time) : (float)(1.0f / 60.0f);
    bd->Time = current_time;

    // Setup inputs
    ALLEGRO_KEYBOARD_STATE keys;
    al_get_keyboard_state(&keys);
    io.KeyCtrl = al_key_down(&keys, ALLEGRO_KEY_LCTRL) || al_key_down(&keys, ALLEGRO_KEY_RCTRL);
    io.KeyShift = al_key_down(&keys, ALLEGRO_KEY_LSHIFT) || al_key_down(&keys, ALLEGRO_KEY_RSHIFT);
    io.KeyAlt = al_key_down(&keys, ALLEGRO_KEY_ALT) || al_key_down(&keys, ALLEGRO_KEY_ALTGR);
    io.KeySuper = al_key_down(&keys, ALLEGRO_KEY_LWIN) || al_key_down(&keys, ALLEGRO_KEY_RWIN);

    ImGui_ImplAllegro5_UpdateMouseCursor();
}