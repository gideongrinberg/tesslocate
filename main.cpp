#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <curl/curl.h>
#include "external/cxxopts.h"
#include "external/csv.h"
#include <nlohmann/json.hpp>
#include <s2/s2point.h>
#include <s2/s2latlng.h>
#include <s2/s2polygon.h>
#include <s2/s2contains_point_query.h>

#ifdef USE_OPENMP
#include <omp.h>
#endif

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

std::string cache_dir() {
#if defined(_WIN32)
    const char* localAppData = getenv("LOCALAPPDATA");
    return localAppData ? std::string(localAppData) : ".";
#else
    if (const char *xdg = getenv("XDG_CACHE_HOME")) return {xdg};
    const char *home = getenv("HOME");
    return home ? std::string(home) + "/.cache/" : ".";
#endif
}

// Used in download_footprints b/c libCURL needs a C-style callback.
static size_t write_callback(const char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    auto resp = static_cast<std::string *>(userdata);
    resp->append(ptr, total_size);
    return total_size;
}

// Download the footprint cache file from S3.
std::string download_footprints() {
    std::string response;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *hnd = curl_easy_init();
    if (!hnd) {
        std::cerr << "curl_easy_init() failed\n";
        return "";
    }

    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(hnd, CURLOPT_URL,
                     "https://stpubdata.s3.amazonaws.com/tess/public/footprints/tess_ffi_footprint_cache.json");
    CURLcode ret = curl_easy_perform(hnd);
    if (ret != CURLE_OK) {
        throw std::runtime_error("Failed to download cache file. Error code: " + std::string(curl_easy_strerror(ret)));
    }

    curl_easy_cleanup(hnd);
    return response;
}

// Loads the footprint cache file or downloads if it doesn't exist.
json load_footprints() {
    std::string dir = cache_dir();
    std::string filename = "tess_ffi_footprint_cache.json";
    std::filesystem::path p(dir);
    std::filesystem::create_directories(p);
    p = p / filename;

    std::string footprints;
    if (!std::filesystem::exists(p)) {
        std::cout << "Footprint cache not found, downloading." << std::endl;
        footprints = download_footprints();
        std::ofstream file(p);
        if (!file.is_open()) {
            std::cerr << "Failed to open footprint cache (" << p.string() << "). Proceeding anyway." << std::endl;
        } else {
            file << footprints;
            std::cout << "Saved footprints to cache file." << std::endl;
        }
    } else {
        std::cout << "Using cached FFI footprints." << std::endl;
        std::ifstream file(p);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open cached FFI footprints: " + p.string());
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        footprints = buffer.str();
    }

    return json::parse(footprints);
}

// Create an S2 point from a right ascension and declination
S2Point radec_point(double ra, double dec) {
    ra = (ra > 180.0) ? ra - 360.0 : ra; // normalize ra to [-180, 180]
    const auto ll = S2LatLng::FromDegrees(dec, ra);
    return ll.ToPoint();
}

// Create an S2 polygon from the text format used in the footprint cache:
// POLYGON RA1 DEC1 RA2 DEC2...
std::unique_ptr<S2Polygon> load_region(const std::string &region) {
    std::vector<std::string> parts;
    std::istringstream ss(region);
    std::string item;
    while (std::getline(ss, item, ' ')) {
        parts.push_back(item);
    }

    if (parts[0] != "POLYGON") {
        std::cerr << "Invalid region:" << region << std::endl;
        return nullptr;
    }

    if ((parts.size() - 1) % 2 != 0) {
        std::cerr << "Invalid number of coordinates:" << region << std::endl;
        return nullptr;
    }

    std::vector<S2Point> points;
    for (int i = 1; i < parts.size() - 1; i += 2) {
        double ra, dec;
        try {
            ra = std::stod(parts[i]);
            dec = std::stod(parts[i + 1]);
            ra = (ra > 180.0) ? ra - 360.0 : ra; // normalize ra to [-180, 180]
        } catch (const std::invalid_argument &e) {
            std::cerr << "Invalid coordinate:" << parts[i] << ", " << parts[i + 1] << std::endl;
            return nullptr;
        }

        auto ll = S2LatLng::FromDegrees(dec, ra);
        points.push_back(ll.ToPoint());
    }

    if (points.size() >= 2 && points.front() == points.back()) {
        points.pop_back(); // remove duplicate point
    }

    auto loop = std::make_unique<S2Loop>(points);
    loop->Normalize();

    return std::make_unique<S2Polygon>(std::move(loop));
}

class IndexedPolygons {
    MutableS2ShapeIndex index;
    std::vector<std::unique_ptr<S2Polygon> > polygons;
    std::vector<std::string> names;

public:
    static IndexedPolygons load() {
        json footprints = load_footprints();
        assert(footprints["obs_id"].size() == footprints["s_region"].size());

        IndexedPolygons res;
        res.names = footprints["obs_id"].get<std::vector<std::string> >();

        for (const auto &region: footprints["s_region"]) {
            auto poly = load_region(region);
            res.index.Add(std::make_unique<S2Polygon::Shape>(poly.get()));
            res.polygons.push_back(std::move(poly));
        }

        return res;
    }

    std::vector<std::string> search(const S2Point &point) {
        std::vector<std::string> res;
        auto query = S2ContainsPointQuery(&this->index);
        query.VisitContainingShapes(point, [this, &res](const auto &shape) {
            res.emplace_back(this->names[shape->id()]);
            return true;
        });

        return res;
    }
};

struct Target {
    std::string ID;
    double ra;
    double dec;
    std::vector<std::string> observations;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Target, ID, ra, dec, observations)
};

int main(int argc, char *argv[]) {
    cxxopts::Options options("tesslocate", "Locate targets on TESS FFIs");
    options.add_options()("input", "path to csv with columns ID, ra, dec", cxxopts::value<std::string>())(
        "output", "output file path, either json or csv", cxxopts::value<std::string>());
    options.parse_positional({"input", "output"});
    auto result = options.parse(argc, argv);
    auto input = result["input"].as<std::string>();
    auto output = result["output"].as<std::string>();

    if (!std::filesystem::exists(input)) {
        std::cout << "File " << argv[1] << " does not exist." << std::endl;
        return 1;
    }

    std::string format;
    if (output.substr(output.length() - 4, 4) == "json") {
        format = "json";
    } else if (output.substr(output.length() - 3, 3) == "csv") {
        format = "csv";
    } else {
        std::cerr << "Invalid output format." << std::endl;
        return 1;
    }

    IndexedPolygons index = IndexedPolygons::load();
    csv::CSVReader reader(input);
    std::vector<csv::CSVRow> rows;
    for (const auto &row: reader) {
        rows.push_back(row);
    }

    std::vector<Target> results(rows.size());
    std::atomic<int> current_iteration = 0;
#ifdef USE_OPENMP
#pragma omp parallel for
#endif
    for (int i = 0; i < rows.size(); ++i) {
        auto row = rows[i];
        Target t;
        t.ID = row["ID"].get<std::string>();
        t.ra = row["ra"].get<double>();
        t.dec = row["dec"].get<double>();
        t.observations = index.search(radec_point(t.ra, t.dec));
        results[i] = t;

        ++current_iteration;
        if (current_iteration % 100 == 0) {
#ifdef USE_OPENMP
#pragma omp critical
            std::cout << "\rProgress: " << current_iteration << "/" << rows.size();
#endif
        }
    }
    std::cout << std::endl;

    if (format == "json") {
        std::cout << "Writing results to json." << std::endl;
        ojson j = results;
        std::ofstream file(output);
        file << j.dump(4);
        std::cout << "Wrote results to " << output << "." << std::endl;
        return 0;
    }

    if (format == "csv") {
        std::cout << "Writing results to csv." << std::endl;
        std::ofstream file(output);
        file << "ID,ra,dec,sector,camera,ccd" << std::endl;
        for (const auto &t: results) {
            for (const auto &obs: t.observations) {
                file << t.ID << "," << t.ra << "," << t.dec << "," <<
                    std::stoi(obs.substr(6, 4)) << "," << obs.substr(11, 1) << "," << obs.substr(13, 1) << std::endl;
            }
        }

        std::cout << "Wrote results to " << output << "." << std::endl;
        return 0;
    }

    return 0;
}
