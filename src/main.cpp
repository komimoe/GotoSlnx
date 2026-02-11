#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cxxopts.hpp>
#include <fmt/format.h>
#include <tinyxml2.h>

namespace fs = std::filesystem;

namespace
{

    constexpr std::string_view kSolutionFolderTypeGuid = "{66A26720-8FB5-11D2-AA7E-00C04F688DDE}";

    struct ProjectConfigMapping
    {
        std::string projectBuildType;
        std::string projectPlatform;
        bool        hasActive = false;
        bool        build     = false;
        bool        buildSet  = false;
        bool        deploy    = false;
        bool        deploySet = false;
    };

    struct ProjectEntry
    {
        std::string                                 typeGuid;
        std::string                                 name;
        std::string                                 path;
        std::string                                 guid;
        std::vector<std::string>                    dependencies;
        std::vector<std::string>                    solutionItems;
        std::map<std::string, ProjectConfigMapping> configMap;
        bool                                        isSolutionFolder = false;
    };

    struct SolutionData
    {
        std::vector<ProjectEntry>                    projects;
        std::unordered_map<std::string, std::string> guidToPath;
        std::unordered_map<std::string, std::string> guidToName;
        std::unordered_map<std::string, std::string> nestedProjects;
        std::set<std::string>                        solutionConfigs;
        std::set<std::string>                        buildTypes;
        std::set<std::string>                        platforms;
    };

    std::string Trim(std::string_view input)
    {
        size_t start = 0;
        while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
            ++start;
        }
        size_t end = input.size();
        while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
            --end;
        }
        return std::string(input.substr(start, end - start));
    }

    bool StartsWith(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    std::vector<std::string> SplitOnce(std::string_view text, char delimiter)
    {
        auto pos = text.find(delimiter);
        if (pos == std::string_view::npos) {
            return { std::string(text) };
        }
        return { std::string(text.substr(0, pos)), std::string(text.substr(pos + 1)) };
    }

    std::pair<std::string, std::string> SplitConfig(const std::string& config)
    {
        auto parts = SplitOnce(config, '|');
        if (parts.size() == 1) {
            return { Trim(parts[0]), std::string() };
        }
        return { Trim(parts[0]), Trim(parts[1]) };
    }

    std::optional<ProjectEntry> ParseProjectHeader(const std::string& line)
    {
        static const std::regex pattern(R"SLN(^Project\("\{([^}]+)\}"\)\s*=\s*"([^"]+)",\s*"([^"]+)",\s*"\{([^}]+)\}"\s*$)SLN");
        std::smatch             match;
        if (!std::regex_match(line, match, pattern)) {
            return std::nullopt;
        }
        ProjectEntry entry;
        entry.typeGuid         = fmt::format("{{{}}}", match[1].str());
        entry.name             = match[2].str();
        entry.path             = match[3].str();
        entry.guid             = fmt::format("{{{}}}", match[4].str());
        entry.isSolutionFolder = fmt::format("{{{}}}", match[1].str()) == kSolutionFolderTypeGuid;
        return entry;
    }

    void ParseSolutionConfiguration(const std::string& line, SolutionData& data)
    {
        auto parts = SplitOnce(line, '=');
        if (parts.empty()) {
            return;
        }
        std::string left = Trim(parts[0]);
        if (left.empty()) {
            return;
        }
        data.solutionConfigs.insert(left);
        auto configParts = SplitConfig(left);
        if (!configParts.first.empty()) {
            data.buildTypes.insert(configParts.first);
        }
        if (!configParts.second.empty()) {
            data.platforms.insert(configParts.second);
        }
    }

    void ParseProjectConfiguration(const std::string& line, SolutionData& data)
    {
        auto parts = SplitOnce(line, '=');
        if (parts.size() < 2) {
            return;
        }
        std::string left  = Trim(parts[0]);
        std::string right = Trim(parts[1]);

        if (!StartsWith(left, "{")) {
            return;
        }

        auto guidEnd = left.find('}');
        if (guidEnd == std::string::npos) {
            return;
        }
        std::string guid      = left.substr(0, guidEnd + 1);
        std::string remainder = left.substr(guidEnd + 1);
        if (remainder.empty() || remainder[0] != '.') {
            return;
        }
        remainder = remainder.substr(1);

        auto lastDot = remainder.rfind('.');
        if (lastDot == std::string::npos) {
            return;
        }
        std::string solutionConfig = remainder.substr(0, lastDot);
        std::string suffix         = remainder.substr(lastDot + 1);

        data.solutionConfigs.insert(solutionConfig);

        auto projectIter
            = std::find_if(data.projects.begin(), data.projects.end(), [&](const ProjectEntry& entry) { return entry.guid == guid; });
        if (projectIter == data.projects.end()) {
            return;
        }

        ProjectConfigMapping& mapping = projectIter->configMap[solutionConfig];
        if (suffix == "ActiveCfg") {
            auto configParts         = SplitConfig(right);
            mapping.projectBuildType = configParts.first;
            mapping.projectPlatform  = configParts.second;
            mapping.hasActive        = true;
        } else if (StartsWith(suffix, "Build")) {
            mapping.build    = true;
            mapping.buildSet = true;
            if (!right.empty() && !mapping.hasActive) {
                auto configParts         = SplitConfig(right);
                mapping.projectBuildType = configParts.first;
                mapping.projectPlatform  = configParts.second;
                mapping.hasActive        = true;
            }
        } else if (StartsWith(suffix, "Deploy")) {
            mapping.deploy    = true;
            mapping.deploySet = true;
            if (!right.empty() && !mapping.hasActive) {
                auto configParts         = SplitConfig(right);
                mapping.projectBuildType = configParts.first;
                mapping.projectPlatform  = configParts.second;
                mapping.hasActive        = true;
            }
        }
    }

    void ParseNestedProject(const std::string& line, SolutionData& data)
    {
        auto parts = SplitOnce(line, '=');
        if (parts.size() < 2) {
            return;
        }
        std::string child  = Trim(parts[0]);
        std::string parent = Trim(parts[1]);
        if (child.empty() || parent.empty()) {
            return;
        }
        data.nestedProjects[child] = parent;
    }

    std::string NormalizeFolderPath(const std::vector<std::string>& segments)
    {
        std::string path = "/";
        for (const auto& segment : segments) {
            if (!segment.empty()) {
                path += segment;
                if (path.back() != '/') {
                    path += '/';
                }
            }
        }
        return path;
    }

    std::string ResolveFolderPath(const std::string& folderGuid, const SolutionData& data,
        std::unordered_map<std::string, std::string>& cache, std::unordered_map<std::string, bool>& visiting)
    {
        auto cached = cache.find(folderGuid);
        if (cached != cache.end()) {
            return cached->second;
        }
        if (visiting[folderGuid]) {
            return "/";
        }
        visiting[folderGuid] = true;

        std::vector<std::string> segments;
        auto                     nameIter = data.guidToName.find(folderGuid);
        if (nameIter != data.guidToName.end()) {
            segments.push_back(nameIter->second);
        }
        auto parentIter = data.nestedProjects.find(folderGuid);
        if (parentIter != data.nestedProjects.end()) {
            std::string parentPath = ResolveFolderPath(parentIter->second, data, cache, visiting);
            if (parentPath != "/") {
                std::string trimmed = parentPath.substr(1);
                if (!trimmed.empty() && trimmed.back() == '/') {
                    trimmed.pop_back();
                }
                if (!trimmed.empty()) {
                    std::vector<std::string> parentSegments;
                    size_t                   start = 0;
                    while (start < trimmed.size()) {
                        auto slash = trimmed.find('/', start);
                        if (slash == std::string::npos) {
                            parentSegments.push_back(trimmed.substr(start));
                            break;
                        }
                        parentSegments.push_back(trimmed.substr(start, slash - start));
                        start = slash + 1;
                    }
                    parentSegments.insert(parentSegments.end(), segments.begin(), segments.end());
                    segments = std::move(parentSegments);
                }
            } else if (segments.empty() && nameIter != data.guidToName.end()) {
                segments.push_back(nameIter->second);
            }
        }

        std::string path     = NormalizeFolderPath(segments);
        cache[folderGuid]    = path;
        visiting[folderGuid] = false;
        return path;
    }

    fs::path ResolveInputPath(const std::string& input)
    {
        fs::path path(input);
        if (fs::is_directory(path)) {
            std::vector<fs::path> slnFiles;
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.path().extension() == ".sln") {
                    slnFiles.push_back(entry.path());
                }
            }
            if (slnFiles.empty()) {
                throw std::runtime_error("目录中未找到 .sln 文件。请指定具体的 .sln 文件路径。");
            }
            if (slnFiles.size() > 1) {
                throw std::runtime_error("目录中存在多个 .sln 文件，请指定要转换的文件。");
            }
            return slnFiles.front();
        }
        return path;
    }

    SolutionData ParseSln(const fs::path& slnPath)
    {
        std::ifstream input(slnPath);
        if (!input) {
            throw std::runtime_error("无法打开 .sln 文件。");
        }

        SolutionData data;
        std::string  line;
        bool         inProject             = false;
        bool         inProjectDependencies = false;
        bool         inSolutionItems       = false;
        bool         inGlobalSection       = false;
        std::string  currentGlobalSection;

        while (std::getline(input, line)) {
            std::string trimmed = Trim(line);
            if (trimmed.empty()) {
                continue;
            }

            if (!inProject && StartsWith(trimmed, "Project(")) {
                auto projectOpt = ParseProjectHeader(trimmed);
                if (projectOpt) {
                    data.projects.push_back(*projectOpt);
                    ProjectEntry& entry         = data.projects.back();
                    data.guidToName[entry.guid] = entry.name;
                    if (!entry.isSolutionFolder) {
                        data.guidToPath[entry.guid] = entry.path;
                    }
                    inProject = true;
                }
                continue;
            }

            if (inProject) {
                if (StartsWith(trimmed, "ProjectSection(")) {
                    if (trimmed.find("ProjectDependencies") != std::string::npos) {
                        inProjectDependencies = true;
                    } else if (trimmed.find("SolutionItems") != std::string::npos) {
                        inSolutionItems = true;
                    }
                    continue;
                }
                if (StartsWith(trimmed, "EndProjectSection")) {
                    inProjectDependencies = false;
                    inSolutionItems       = false;
                    continue;
                }
                if (StartsWith(trimmed, "EndProject")) {
                    inProject             = false;
                    inProjectDependencies = false;
                    inSolutionItems       = false;
                    continue;
                }

                if (inProjectDependencies) {
                    auto parts = SplitOnce(trimmed, '=');
                    if (parts.size() >= 2) {
                        std::string dep = Trim(parts[0]);
                        if (!dep.empty()) {
                            data.projects.back().dependencies.push_back(dep);
                        }
                    }
                } else if (inSolutionItems) {
                    auto parts = SplitOnce(trimmed, '=');
                    if (parts.size() >= 2) {
                        std::string item = Trim(parts[1]);
                        if (!item.empty()) {
                            data.projects.back().solutionItems.push_back(item);
                        }
                    }
                }
                continue;
            }

            if (StartsWith(trimmed, "GlobalSection(")) {
                inGlobalSection = true;
                auto start      = trimmed.find('(');
                auto end        = trimmed.find(')');
                if (start != std::string::npos && end != std::string::npos && end > start + 1) {
                    currentGlobalSection = trimmed.substr(start + 1, end - start - 1);
                } else {
                    currentGlobalSection.clear();
                }
                continue;
            }
            if (StartsWith(trimmed, "EndGlobalSection")) {
                inGlobalSection = false;
                currentGlobalSection.clear();
                continue;
            }

            if (inGlobalSection) {
                if (currentGlobalSection == "SolutionConfigurationPlatforms") {
                    ParseSolutionConfiguration(trimmed, data);
                } else if (currentGlobalSection == "ProjectConfigurationPlatforms") {
                    ParseProjectConfiguration(trimmed, data);
                } else if (currentGlobalSection == "NestedProjects") {
                    ParseNestedProject(trimmed, data);
                }
            }
        }

        for (auto& project : data.projects) {
            for (auto& [solutionConfig, mapping] : project.configMap) {
                if (mapping.hasActive) {
                    if (!mapping.buildSet) {
                        mapping.build    = false;
                        mapping.buildSet = true;
                    }
                    if (!mapping.deploySet) {
                        mapping.deploy    = false;
                        mapping.deploySet = true;
                    }
                }
            }
        }

        return data;
    }

    void AppendBuildTypesAndPlatforms(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* root, const SolutionData& data)
    {
        if (data.buildTypes.empty() && data.platforms.empty()) {
            return;
        }

        auto* configs = doc.NewElement("Configurations");
        root->InsertEndChild(configs);

        for (const auto& buildType : data.buildTypes) {
            auto* elem = doc.NewElement("BuildType");
            elem->SetAttribute("Name", buildType.c_str());
            configs->InsertEndChild(elem);
        }
        for (const auto& platform : data.platforms) {
            auto* elem = doc.NewElement("Platform");
            elem->SetAttribute("Name", platform.c_str());
            configs->InsertEndChild(elem);
        }
    }

    void AppendProjectXml(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const ProjectEntry& project, const SolutionData& data)
    {
        auto* projectElem = doc.NewElement("Project");
        projectElem->SetAttribute("Path", project.path.c_str());
        projectElem->SetAttribute("Id", project.guid.c_str());

        std::string filename = fs::path(project.path).stem().string();
        if (!project.name.empty() && project.name != filename) {
            projectElem->SetAttribute("DisplayName", project.name.c_str());
        }

        for (const auto& depGuid : project.dependencies) {
            auto depIter = data.guidToPath.find(depGuid);
            if (depIter == data.guidToPath.end()) {
                continue;
            }
            auto* depElem = doc.NewElement("BuildDependency");
            depElem->SetAttribute("Project", depIter->second.c_str());
            projectElem->InsertEndChild(depElem);
        }

        for (const auto& [solutionConfig, mapping] : project.configMap) {
            if (!mapping.hasActive) {
                continue;
            }

            if (!mapping.projectBuildType.empty()) {
                auto* buildTypeElem = doc.NewElement("BuildType");
                buildTypeElem->SetAttribute("Solution", solutionConfig.c_str());
                buildTypeElem->SetAttribute("Project", mapping.projectBuildType.c_str());
                projectElem->InsertEndChild(buildTypeElem);
            }

            if (!mapping.projectPlatform.empty()) {
                auto* platformElem = doc.NewElement("Platform");
                platformElem->SetAttribute("Solution", solutionConfig.c_str());
                platformElem->SetAttribute("Project", mapping.projectPlatform.c_str());
                projectElem->InsertEndChild(platformElem);
            }

            if (mapping.buildSet) {
                auto* buildElem = doc.NewElement("Build");
                buildElem->SetAttribute("Solution", solutionConfig.c_str());
                buildElem->SetAttribute("Project", mapping.build ? "true" : "false");
                projectElem->InsertEndChild(buildElem);
            }

            if (mapping.deploySet) {
                auto* deployElem = doc.NewElement("Deploy");
                deployElem->SetAttribute("Solution", solutionConfig.c_str());
                deployElem->SetAttribute("Project", mapping.deploy ? "true" : "false");
                projectElem->InsertEndChild(deployElem);
            }
        }

        parent->InsertEndChild(projectElem);
    }

    void WriteSlnx(const fs::path& outputPath, const SolutionData& data)
    {
        tinyxml2::XMLDocument doc;
        doc.InsertEndChild(doc.NewDeclaration());

        auto* root = doc.NewElement("Solution");
        doc.InsertEndChild(root);

        AppendBuildTypesAndPlatforms(doc, root, data);

        std::unordered_map<std::string, std::string>                      folderPathCache;
        std::unordered_map<std::string, bool>                             visiting;
        std::unordered_map<std::string, std::vector<const ProjectEntry*>> projectsByFolder;
        std::unordered_map<std::string, const ProjectEntry*>              folderEntries;

        for (const auto& project : data.projects) {
            if (project.isSolutionFolder) {
                folderEntries[project.guid] = &project;
                continue;
            }
            auto nestedIter = data.nestedProjects.find(project.guid);
            if (nestedIter != data.nestedProjects.end()) {
                std::string folderPath = ResolveFolderPath(nestedIter->second, data, folderPathCache, visiting);
                projectsByFolder[folderPath].push_back(&project);
            } else {
                projectsByFolder["/"].push_back(&project);
            }
        }

        std::map<std::string, std::vector<std::string>> filesByFolder;
        for (const auto& [guid, folderProject] : folderEntries) {
            std::string folderPath    = ResolveFolderPath(guid, data, folderPathCache, visiting);
            filesByFolder[folderPath] = folderProject->solutionItems;
        }

        for (const auto& [folderPath, files] : filesByFolder) {
            auto* folderElem = doc.NewElement("Folder");
            folderElem->SetAttribute("Name", folderPath.c_str());

            for (const auto& filePath : files) {
                auto* fileElem = doc.NewElement("File");
                fileElem->SetAttribute("Path", filePath.c_str());
                folderElem->InsertEndChild(fileElem);
            }

            auto projIter = projectsByFolder.find(folderPath);
            if (projIter != projectsByFolder.end()) {
                for (const auto* project : projIter->second) {
                    AppendProjectXml(doc, folderElem, *project, data);
                }
            }

            root->InsertEndChild(folderElem);
        }

        auto rootProjectsIter = projectsByFolder.find("/");
        if (rootProjectsIter != projectsByFolder.end()) {
            for (const auto* project : rootProjectsIter->second) {
                AppendProjectXml(doc, root, *project, data);
            }
        }

        if (doc.SaveFile(outputPath.string().c_str()) != tinyxml2::XML_SUCCESS) {
            throw std::runtime_error("写入 .slnx 文件失败。");
        }
    }

}  // namespace

int main(int argc, char** argv)
{
    try {
        cxxopts::Options options("goto-slnx", "一键将 .sln 转换为 .slnx");
        options.add_options()("i,input", "输入 .sln 路径（或包含单个 .sln 的目录）", cxxopts::value<std::string>())("o,output",
            "输出 .slnx 路径（默认同名）", cxxopts::value<std::string>())("f,force", "覆盖已有 .slnx 文件",
            cxxopts::value<bool>()->default_value("false"))("h,help", "显示帮助");

        auto result = options.parse(argc, argv);
        if (result.count("help") || !result.count("input")) {
            fmt::print("{}\n", options.help());
            return 0;
        }

        fs::path inputPath = ResolveInputPath(result["input"].as<std::string>());
        if (inputPath.extension() != ".sln") {
            throw std::runtime_error("输入文件不是 .sln。");
        }

        fs::path outputPath;
        if (result.count("output")) {
            outputPath = result["output"].as<std::string>();
        } else {
            outputPath = inputPath;
            outputPath.replace_extension(".slnx");
        }

        if (fs::exists(outputPath) && !result["force"].as<bool>()) {
            throw std::runtime_error("输出 .slnx 已存在，使用 --force 覆盖。");
        }

        SolutionData data = ParseSln(inputPath);
        WriteSlnx(outputPath, data);

        fmt::print("已生成: {}\n", outputPath.string());
        return 0;
    } catch (const std::exception& ex) {
        fmt::print(stderr, "错误: {}\n", ex.what());
        return 1;
    }
}
