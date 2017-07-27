#include "primops.hh"
#include "eval-inline.hh"
#include "download.hh"
#include "store-api.hh"
#include "pathlocks.hh"

namespace nix {

Path exportGit(ref<Store> store, const std::string & uri, const std::string & rev)
{
    if (!isUri(uri))
        throw EvalError(format("‘%s’ is not a valid URI") % uri);

    Path cacheDir = getCacheDir() + "/nix/git";

    if (!pathExists(cacheDir)) {
        createDirs(cacheDir);
        runProgram("git", true, { "init", "--bare", cacheDir });
    }

    //Activity act(*logger, lvlInfo, format("fetching Git repository ‘%s’") % uri);

    std::string localRef = hashString(htSHA256, fmt("%s-%s", uri, rev)).to_string(Base32, false);

    Path localRefFile = cacheDir + "/refs/heads/" + localRef;

    runProgram("git", true, { "-C", cacheDir, "fetch", "--force", uri, rev + ":" + localRef });

    std::string commitHash = chomp(readFile(localRefFile));

    printTalkative("using revision %s of repo ‘%s’", uri, commitHash);

    Path storeLink = cacheDir + "/" + commitHash + ".link";
    PathLocks storeLinkLock({storeLink}, fmt("waiting for lock on ‘%1%’...", storeLink));

    if (pathExists(storeLink)) {
        auto storePath = readLink(storeLink);
        store->addTempRoot(storePath);
        if (store->isValidPath(storePath)) {
            return storePath;
        }
    }

    // FIXME: should pipe this, or find some better way to extract a
    // revision.
    auto tar = runProgram("git", true, { "-C", cacheDir, "archive", commitHash });

    Path tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, true);

    runProgram("tar", true, { "x", "-C", tmpDir }, tar);

    auto storePath = store->addToStore("git-export", tmpDir);

    replaceSymlink(storePath, storeLink);

    return storePath;
}

static void prim_fetchgit(EvalState & state, const Pos & pos, Value * * args, Value & v)
{
    // FIXME: cut&paste from fetch().
    if (state.restricted) throw Error("‘fetchgit’ is not allowed in restricted mode");

    std::string url;
    std::string rev = "master";

    state.forceValue(*args[0]);

    if (args[0]->type == tAttrs) {

        state.forceAttrs(*args[0], pos);

        for (auto & attr : *args[0]->attrs) {
            string name(attr.name);
            if (name == "url") {
                PathSet context;
                url = state.coerceToString(*attr.pos, *attr.value, context, false, false);
                if (hasPrefix(url, "/")) url = "file://" + url;
            } else if (name == "rev")
                rev = state.forceStringNoCtx(*attr.value, *attr.pos);
            else
                throw EvalError("unsupported argument ‘%s’ to ‘fetchgit’, at %s", attr.name, *attr.pos);
        }

        if (url.empty())
            throw EvalError(format("‘url’ argument required, at %1%") % pos);

    } else
        url = state.forceStringNoCtx(*args[0], pos);

    Path storePath = exportGit(state.store, url, rev);

    mkString(v, storePath, PathSet({storePath}));
}

static RegisterPrimOp r("__fetchgit", 1, prim_fetchgit);

}
