#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace odv {

// PROGRESSIVE DISCLOSURE. A skill is a folder with a SKILL.md (frontmatter:
// name, description; body: the instructions) plus optional helper files:
//
//   <project>/.ollamadev/skills/<name>/SKILL.md   project skills (win)
//   ~/.ollamadev/skills/<name>/SKILL.md           global skills
//
// Only name+description ever reach the system prompt (Skills::catalog()). The
// model pulls a body on demand with the `skill` tool. That is the entire point:
// inlining bodies would put every skill's full instructions in every request and
// blow the context budget of exactly the small local models this exists to help.
struct Skill {
    QString name;
    QString description;
    QString body;       // instructions WITHOUT the frontmatter; loaded on demand
    QString dir;        // on-disk folder; empty for a built-in
    QStringList files;  // helper files sitting next to SKILL.md
    bool builtin = false;
    bool installed = false;  // (browse/search) already present on disk?
    QString source;          // (browse) registry it was discovered in

    bool isNull() const { return name.isEmpty(); }
};

struct SkillInstall {
    QStringList installed;
    QStringList messages;
};

class Skills {
public:
    static QStringList baseDirs();
    static QString homeDir();  // ~/.ollamadev/skills — where installs land

    // Every skill on disk, project overriding global, sorted by name.
    static QVector<Skill> all();
    static QStringList names();

    // Same, but for an EXPLICIT project root. A crew coder's project is its
    // sandbox, and its system prompt is built before its thread root is set, so
    // the caller must be able to say which root it means rather than depending on
    // when the thread-local happens to be assigned.
    static QVector<Skill> allIn(const QString& projectRoot);
    static QString catalogFor(const QString& projectRoot);

    // Full skill (body + helper files) by name. Falls back to the built-in
    // library, so a built-in is viewable before a crew run materialises it.
    // Returns a null Skill (isNull()) when there is no such skill.
    static Skill get(const QString& name);

    // Built-ins NOT shadowed by an on-disk skill of the same name.
    static QVector<Skill> builtins();
    // Disk skills first, then built-ins — what a skills manager UI lists.
    static QVector<Skill> listForManager();

    // "- name: description" lines for the system prompt. Empty when there are none.
    static QString catalog();

    // ---- registry / discovery ---------------------------------------------
    // A registry is just somewhere to DISCOVER skills before installing them:
    // the local registry dir plus any dirs listed in config `skills.registries`.
    // Git/archive URLs are install-only — you cannot list one without fetching it.
    static QString registryDir();
    static QStringList registries();
    static QVector<Skill> browse();
    static QVector<Skill> search(const QString& query);
    static SkillInstall addFromRegistry(const QString& name, bool force = false);

    // Install from a local directory, a git URL, or a .tar.gz/.tgz/.zip (local
    // path or http(s) URL). Every folder containing a SKILL.md under the source
    // is copied to ~/.ollamadev/skills/<name>.
    //
    // SECURITY: `source` is attacker-influenced (it can come from a shared pack
    // or a registry listing). Every external command runs through QProcess with
    // an ARGUMENT ARRAY and never a composed shell string, so a source like
    // `foo; rm -rf ~` is one absurd argument, not two commands. Deletions use
    // Qt, never a shelled-out `rm -rf`.
    static SkillInstall install(const QString& source, bool force = false);

    // Package a skill folder as a shareable tarball. Returns the path, or empty.
    static QString exportSkill(const QString& name, const QString& out = {});

    // Delete an installed skill. Built-ins have no folder, so they never match.
    static bool remove(const QString& name);

    // Scaffold ~/.ollamadev/skills/<slug>/SKILL.md. Returns the SKILL.md path.
    static QString scaffold(const QString& name);

    // Create/overwrite a user skill. Returns the slug, or empty on failure.
    static QString save(const QString& name, const QString& description, const QString& body);

    static QString slugify(const QString& name);
};

// ---------------------------------------------------------------------------
// CREW TEAM SKILLS — the built-in starter library.
//
// A crew carries a --focus describing its stack. We match that text against each
// starter's trigger keywords and materialise the winners into every coder's
// SANDBOX (<sandbox>/.ollamadev/skills/<name>/SKILL.md). The coder, whose tools
// are already rooted at that sandbox, then discovers them exactly like project
// skills and pulls each body on demand. A user skill of the same name always
// wins — we never clobber one somebody customised.
// ---------------------------------------------------------------------------
struct SkillSpec {
    QString name;
    QStringList triggers;
    QString description;
    QString body;
};

class CrewSkills {
public:
    // Capability skills — these are the ones focus-matched into a crew run.
    static const QVector<SkillSpec>& library();
    // One starter per crew TEAM. Listed/editable in the manager, but NEVER
    // focus-matched: adding a team starter must not silently change how an
    // existing crew behaves.
    static const QVector<SkillSpec>& teamLibrary();
    static QVector<SkillSpec> allBuiltins();

    static QVector<SkillSpec> byNames(const QStringList& names);

    // Starters whose triggers occur in `focus`, longest (most specific) trigger
    // first, capped.
    static QVector<SkillSpec> forFocus(const QString& focus, int cap = 5);

    // Forced-by-name skills (never dropped by the cap) + the focus-matched ones.
    static QVector<SkillSpec> resolve(const QString& focus, const QStringList& force = {},
                                      int cap = 5);

    // Write each skill to <baseDir>/.ollamadev/skills/<name>/SKILL.md. Skips any
    // the user already defines globally, and any already present at the target.
    // Returns the names now available there.
    static QStringList materialize(const QVector<SkillSpec>& skills, const QString& baseDir);
};

// ---------------------------------------------------------------------------
// CREW ROLES — the persona catalog the Director assigns per subtask. Built-ins
// always exist; user roles are plain JSON at ~/.ollamadev/crew-roles/<name>.json
// and may override a built-in by reusing its name.
// ---------------------------------------------------------------------------
struct CrewRole {
    QString name;
    QString desc;    // what the Director sees when choosing
    QString prompt;  // the persona injected into the coder's system prompt
    QString model;   // optional pin; empty = the crew's coder model
    bool readOnly = false;
    bool custom = false;
};

class CrewRoles {
public:
    static QString dir();  // ~/.ollamadev/crew-roles
    static QVector<CrewRole> all();
    static QStringList names();

    // Unknown names fall back to 'coder' — the Director hallucinating a role must
    // never strand a subtask without a persona.
    static CrewRole get(const QString& name);

    static QString normName(const QString& name);
    static QString add(const QString& name, const QString& prompt, const QString& desc = {},
                       const QString& model = {}, bool readOnly = false);
    static bool remove(const QString& name);

    // "- name: desc" lines the Director sees.
    static QString catalog();

    // The persona block appended to a coder's system prompt for this role.
    // Empty for a role with no prompt.
    static QString persona(const QString& name);
};

// ---------------------------------------------------------------------------
// CREW PACKS — saved, shareable crew configurations at ~/.ollamadev/crew-packs/.
// A pack bundles only the REUSABLE knobs (focus, per-role backend/model, max,
// amplify, land, research, audit, skills, hosts) — never the one-off task.
// ---------------------------------------------------------------------------
class CrewPacks {
public:
    static QString dir();
    static QStringList keys();  // the reusable knobs a pack may carry

    static QJsonObject builtins();  // name -> pack object

    // Persist the pack-shaped subset of `opts`. Returns the file path.
    static QString save(const QString& name, const QJsonObject& opts);

    // A user pack wins over a built-in of the same name. Empty object if unknown.
    static QJsonObject load(const QString& name);
    static bool exists(const QString& name);

    // name -> one-line summary, for `crew pack list`.
    static QVector<QPair<QString, QString>> all();

    static bool remove(const QString& name);
};

}  // namespace odv
