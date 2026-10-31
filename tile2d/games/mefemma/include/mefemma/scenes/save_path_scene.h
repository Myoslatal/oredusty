// Mefemma - the first-run save-path prompt: one field, one decision, before the game starts.
//
// The game needs to know where its saves live before any scene can open a store or a save list. This scene
// is shown only when redirect.ecfg beside the executable is missing or invalid (and no --save-path was
// given): it asks for a save DIRECTORY, normalises it against the redirect file's own directory, makes the
// directory and records the absolute result in redirect.ecfg - then replaces itself with the scene the shell
// would have started. It is a Scene like any other, so it is driven by a fake GameServices in a test and by
// the real frame loop in the game, and it draws only through the DrawList.
//
// The shell owns the startup order (app.cpp); this scene owns the question, the field, the rules and the
// error shown when the answer cannot be used.
#pragma once

#include <mefemma/path_input.h>
#include <mefemma/scene.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mefemma {

/// The first-run scene: asks for the save directory and hands the game the scene it should start.
class SavePathScene final : public Scene {
public:
    /// What the shell starts once the answer has been accepted. The argument is the normalised ABSOLUTE
    /// save directory, the same value written to redirect.ecfg.
    using StartFactory = std::function<std::unique_ptr<Scene>(std::string_view save_path)>;

    /// \p redirect_path is the redirect.ecfg to read and replace; \p default_save_path is what the field
    /// starts with (the shell's default_save_directory(), or a value a test provides). \p start is called
    /// exactly once, on confirmation, and its result replaces this scene.
    SavePathScene(std::string redirect_path, std::string default_save_path, StartFactory start);

    [[nodiscard]] const char* name() const override { return "first-run"; }

    void on_enter(GameServices& services) override;
    void update(GameServices& services, f32 delta_seconds) override;
    void draw(GameServices& services, DrawList& out) const override;

    // --- what the prompt is showing, for a test and for a game that embeds it -------------------------
    [[nodiscard]] std::string_view text() const { return input_.text(); }
    /// The reason the last confirmation failed (empty when the field has not failed).
    [[nodiscard]] std::string_view error() const { return error_detail_; }
    [[nodiscard]] const std::string& redirect_path() const { return redirect_path_; }
    [[nodiscard]] const std::string& default_save_path() const { return default_save_path_; }

private:
    enum class ErrorKind : t2d::u8 { None, Empty, Failed };

    /// Normalise, create, save, start; on failure keep the scene up with a reason. The field is only read.
    void submit(GameServices& services);
    void clear_error();

    std::string redirect_path_;
    std::string default_save_path_;
    StartFactory start_;
    PathInput input_;
    ErrorKind error_kind_ = ErrorKind::None;
    std::string error_detail_;
};

/// The factory MefemmaApp::on_start() pushes when the run has no configured save directory.
[[nodiscard]] std::unique_ptr<Scene> make_save_path_scene(std::string redirect_path,
                                                          std::string default_save_path,
                                                          SavePathScene::StartFactory start);

} // namespace mefemma
