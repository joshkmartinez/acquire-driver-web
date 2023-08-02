#include <iostream>
#include <filesystem>
#include <fstream>
#include "httplib.h"


// TODO: Check, is this right? Absolute file path, osx?
/*
extern "C" {
    #include "../../acquire-core-libs/src/acquire-core-platform/osx/platform.h"
}
*/

int main() {
    httplib::Server svr;

    // std::cout << "Current working directory: " << std::filesystem::current_path() << std::endl;
    // for(auto & p : std::filesystem::directory_iterator("../public"))
    // std::cout << p << std::endl;


    auto ret = svr.set_mount_point("/", "../public");
    if (!ret) {
        throw std::runtime_error("Failed to set mount point");
    }

    // A semi-complete implementation of range requests for .webm files
    // Serving from /video path
    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Range
    // https://datatracker.ietf.org/doc/html/rfc7233
    svr.Get(R"(/video/(.*\.webm))", [](const httplib::Request& req, httplib::Response& res) {
        std::string range = req.get_header_value("Range");
        std::string filePath = "../public/" + req.matches[1].str();

        std::ifstream file(filePath, std::ifstream::binary | std::ifstream::ate);
        if (!file) {
            res.status = 404;
            res.set_content("File not found", "text/plain");
            return;
        }


        // Get the file size
        std::streamsize size = file.tellg(); // Get position of file pointer (end of file)
        file.seekg(0, std::ios::beg); // Move the file pointer back to the beginning of the file

        // TODO: support end ranges? (ex. -500 = last 500 bytes) Or throw an error? What should happen on non terminating file?
        // TODO: Add support for non byte units?
        std::string contentRange = "bytes 0-" + std::to_string(size-1) + "/" + std::to_string(size); // Default range is the entire file
        
        if (!range.empty()) { // If range header present
            std::size_t equalsPos = range.find('=');
            std::size_t dashPos = range.find('-');
            std::streamsize start = std::stoll(range.substr(equalsPos+1, dashPos-equalsPos-1)); // Start byte of range
            std::streamsize end = (dashPos == std::string::npos || dashPos == range.size() - 1)  // End byte of range
                ? size - 1 
                : std::stoll(range.substr(dashPos+1));

            // Check if the range is not satisfiable
            // TODO: Check if start < 0 ?
            if (start > end || end >= size) {
                res.status = 416; // 416 Range Not Satisfiable
                return;
            }

            size = end-start + 1;
            file.seekg(start);
            // Define the requested & validated range
            contentRange = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
        }

        // Read the file into a buffer
        char* buffer = new char[size];
        file.read(buffer, size);
        std::string content(buffer, size);
        delete[] buffer;

        // Deliver the desired range
        res.status = 206; // 206 Partial Content
        res.set_content(content, "video/webm");
        res.set_header("Content-Range", contentRange.c_str());
    });


    

    svr.Get("/ping", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("pong", "text/plain");
    });

    

    // Catch-all 404 route
    // svr.Get(".*", [](const httplib::Request &, httplib::Response &res) {
    //     res.status = 404;
    //     res.set_content("404 Not Found", "text/plain");
    // });


    svr.listen("0.0.0.0", 8080);

    return 0;
}