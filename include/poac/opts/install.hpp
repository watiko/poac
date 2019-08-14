#ifndef POAC_OPTS_INSTALL_HPP
#define POAC_OPTS_INSTALL_HPP

#include <future>
#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <map>
#include <regex>
#include <optional>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <poac/io.hpp>
#include <poac/core/except.hpp>
#include <poac/core/resolver/resolve.hpp>
#include <poac/util/argparse.hpp>
#include <poac/util/shell.hpp>

namespace poac::opts::install {
    const clap::subcommand cli =
            clap::subcommand("install")
                .about("Install a C++ binary. Default location is $HOME/.poac/bin")
                .arg(clap::opt("verbose", "Use verbose output").short_("v"))
                .arg(clap::opt("quiet", "No output printed to stdout").short_("q"))
                .arg(clap::arg("package").multiple(true))
            ;

    struct Options {
        bool quiet;
        bool verbose;
        std::vector<std::string> package_list;
    };

//    void stream_deps(YAML::Emitter& out, const core::resolver::resolve::NoDuplicateDeps& deps) {
//        out << YAML::Key << "dependencies";
//        out << YAML::Value << YAML::BeginMap;
//
//        for (const auto& [name, package] : deps) {
//            out << YAML::Key << name;
//            out << YAML::Value << YAML::BeginMap;
//
//            out << YAML::Key << "version";
//            out << YAML::Value << package.version;
//
//            out << YAML::Key << "package_type";
//            out << YAML::Value << to_string(package.package_type); // TODO: hash等は不要なのか？？？
//
//            if (!package.dependencies.has_value()) {
//                for (const auto& [name, version] : package.dependencies.value()) {
//                    out << YAML::Key << name;
//                    out << YAML::Value << YAML::BeginMap;
//
//                    out << YAML::Key << "version";
//                    out << YAML::Value << package.version;
//                }
//            }
//            out << YAML::EndMap;
//        }
//        out << YAML::EndMap;
//    }
//
//    void create_lockfile(const std::string& timestamp, const core::resolver::resolve::NoDuplicateDeps& activated_deps) {
//        // Create a poac.lock
//        if (std::ofstream ofs("poac.lock"); ofs) {
//            ofs << "# This file is automatically generated by Poac.\n";
//            ofs << "# Please do not edit this file.\n";
//            YAML::Emitter out;
//            out << YAML::BeginMap;
//
//            out << YAML::Key << "timestamp";
//            out << YAML::Value << timestamp;
//
//            stream_deps(out, activated_deps);
//            ofs << out.c_str() << '\n';
//        }
//    }

    // Copy package to ./deps
    bool copy_to_current(const std::string& from, const std::string& to) {
        const auto from_path = io::path::poac_cache_dir / from;
        const auto to_path = io::path::current_deps_dir / to;
        return io::path::copy_recursive(from_path, to_path);
    }

    void echo_install_status(const bool res, const std::string& name, const std::string& version) {
        const std::string status = name + ": " + version;
        std::cout << '\r' << io::term::clr_line
                  << (res ? io::term::fetched : io::term::fetch_failed) << status << std::endl;
    }

    void
    fetch(const core::resolver::resolve::NoDuplicateDeps& deps, const install::Options& opts) {
        int exists_count = 0;
        for (const auto& [name, package] : deps) {
            const std::string current_name = core::name::to_current(name);
            const std::string cache_name = core::name::to_cache(name, package.version);
            const bool is_cached = core::resolver::resolve::cache::resolve(cache_name);

            if (opts.verbose) {
                std::cout << "NAME: " << name << "\n"
                          << "  VERSION: " <<  package.version << "\n"
                          << "  CACHE_NAME: " << cache_name << "\n"
                          << "  CURRENT_NAME: " << current_name << "\n"
                          << "  IS_CACHED: " << is_cached << "\n"
                          << std::endl;
            }

            if (core::resolver::resolve::current::resolve(current_name)) {
                ++exists_count;
                continue;
            }
            else if (is_cached) {
                const bool res = copy_to_current(cache_name, current_name);
                if (!opts.quiet) {
                    echo_install_status(res, name, package.version);
                }
            }
            else {
                util::shell clone_cmd(core::resolver::resolve::github::clone_command(name, package.version));
                clone_cmd += (io::path::poac_cache_dir / cache_name).string();
                clone_cmd = clone_cmd.to_dev_null().stderr_to_stdout();

                bool result = clone_cmd.exec().has_value(); // true == error
                result = !result && copy_to_current(cache_name, current_name);

                if (!opts.quiet) {
                    echo_install_status(result, name, package.version);
                }
            }
        }
        if (exists_count == static_cast<int>(deps.size())) {
            std::cout << io::term::warning << "Already installed" << std::endl;
        }
    }

    void
    download(const core::resolver::resolve::NoDuplicateDeps& deps, const install::Options& opts) {
        if (!opts.quiet) {
            std::cout << io::term::status << "Fetching..." << std::endl;
            std::cout << std::endl;
        }
        io::path::create_directories(io::path::poac_cache_dir);
        io::path::create_directories(io::path::current_deps_dir);
        fetch(deps, opts);

        if (!opts.quiet) {
            std::cout << std::endl;
            io::term::status_done();
        }
    }

    core::resolver::resolve::NoDuplicateDeps
    resolve_packages(const std::unordered_map<std::string, std::string>& dependencies) {
        core::resolver::resolve::NoDuplicateDeps deps;
        // Even if a package of the same name is written, it is excluded.
        // However, it can not deal with duplication of other information (e.g. version etc.).
        for (const auto& [name, interval] : dependencies) {
            deps.emplace(name, interval);
        }
        return deps;
    }

    core::resolver::resolve::NoDuplicateDeps::value_type
    parse_arg_package(const std::string& v) {
        if (const auto error = core::name::validate_package_name(v)) {
            throw core::except::error( error->what() );
        }

        const std::string NAME = "([a-z|\\d|\\-|_|\\/]*)";
        std::smatch match;
        if (std::regex_match(v, std::regex("^" + NAME + "$"))) { // TODO: 厳しくする
            return { v, { "latest", io::lockfile::PackageType::HeaderOnlyLib, std::nullopt } };
        }
        else if (std::regex_match(v, match, std::regex("^" + NAME + "=(.*)$"))) {
            const auto name = match[1].str();
            const auto interval = match[2].str();
            return { name, { interval, io::lockfile::PackageType::HeaderOnlyLib, std::nullopt } };
        }
        else {
            throw core::except::error("Invalid arguments");
        }
    }

    std::string convert_to_interval(const std::string& version) {
        semver::Version upper(version); // TODO: semverのincrement methodを使う
        upper.major += 1;
        upper.minor = 0;
        upper.patch = 0;
        return ">=" + version + " and <" + upper.get_version();
    }

    std::optional<io::lockfile::Lockfile> // TODO:
    load_lockfile(const install::Options& opts, std::string_view timestamp) {
        if (opts.package_list.empty()) {
            if (const auto lockfile = io::lockfile::load()) {
                if (lockfile.has_value() && lockfile->timestamp == timestamp) {
                    return lockfile.value();
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<core::except::Error>
    install(std::optional<io::config::Config>&& config, install::Options&& opts) {
        std::string timestamp = io::config::get_timestamp();
        const auto lockfile = load_lockfile(opts, timestamp);

        // YAML::Node -> resolver:Deps
        core::resolver::resolve::NoDuplicateDeps deps;
        for (const auto& v : opts.package_list) {
            deps.emplace(parse_arg_package(v));
        }
        if (!lockfile.has_value()) {
            if (const auto dependencies = config->dependencies) {
                const auto resolved_packages = resolve_packages(dependencies.value());
                deps.insert(resolved_packages.begin(), resolved_packages.end());
            }
            else if (opts.package_list.empty()) { // 引数から指定しておらず(poac install)，poac.ymlにdeps keyが存在しない
                return core::except::Error::General{
                    "Required key `dependencies` does not exist in poac.toml.\n"
                    "Please refer to https://doc.poac.pm"
                };
            }
        }

        // resolve dependency
        if (!opts.quiet) {
            std::cout << io::term::status << "Resolving dependencies..." << std::endl;
        }

        if (!lockfile.has_value()) {
            core::resolver::resolve::ResolvedDeps resolved_deps{};
            resolved_deps = core::resolver::resolve::resolve(deps);
            download(resolved_deps.no_duplicate_deps, opts);
//            create_lockfile(timestamp, resolved_deps.no_duplicate_deps);
        } else {
            download(lockfile->dependencies, opts);
        }

        // TODO: Applicationのみ，install可能．~/.poac/bin -> そのまま，buildを実行して良い．

        // TODO: Rewrite poac.yml
//        bool fix_yml = false;
//        for (const auto& d : deps) {
//            if (d.interval == "latest") {
//                node["deps"][d.name] = convert_to_interval(resolved_deps.backtracked[d.name].version);
//                fix_yml = true;
//            }
//        }
//        if (!opts.package_list.empty()) {
//            fix_yml = true;
//            for (const auto& d : deps) {
//                if (d.interval != "latest") {
//                    node["deps"][d.name] = d.interval;
//                }
//            }
//        }
//        if (fix_yml) {
//            if (std::ofstream ofs("poac.yml"); ofs) {
//                ofs << node;
//            }
//            timestamp = io::yaml::load_timestamp();
//        }
//
//        if (!lockfile.has_value()) {
//            create_lockfile(timestamp, resolved_deps.no_duplicate_deps);
//        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<core::except::Error>
    exec(std::future<std::optional<io::config::Config>>&& config, std::vector<std::string>&& args) {
        install::Options opts{};
        opts.quiet = util::argparse::use_rm(args, "-q", "--quite");
        opts.verbose = util::argparse::use_rm(args, "-v", "--verbose") && !opts.quiet;
        args.shrink_to_fit();
        opts.package_list = args;
        return install::install(config.get(), std::move(opts));
    }
} // end namespace
#endif // !POAC_OPTS_INSTALL_HPP
