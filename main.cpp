#include <boost/program_options.hpp>
#include <webp/decode.h>
#include <webp/encode.h>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <print>

namespace po = boost::program_options;

struct Pixel
{
    uint8_t r = 0, g = 0, b = 0;

    friend bool operator==(const Pixel &lhs, const Pixel &rhs)
    {
        return std::tie(lhs.r, lhs.g, lhs.b)
            == std::tie(rhs.r, rhs.g, rhs.b);
    }

    friend bool operator!=(const Pixel &lhs, const Pixel &rhs)
    {
        return !(lhs == rhs);
    }
};

namespace
{
    constexpr Pixel WHITE { .r = 255, .g = 255, .b = 255 };
    constexpr Pixel BLACK { .r = 0, .g = 0, .b = 0 };
}

struct WebPImage
{
    std::vector<uint8_t> data;
    std::vector<Pixel> pixels;
    // size_t data_size = 0;
    int width = 0;
    int height = 0;

    Pixel get_pixel(const size_t x, const size_t y) const
    {
        if(x > width) return { };
        if(y > height) return { };
        return pixels[y * width + x];
    }

    void set_pixel(const size_t x, const size_t y, const Pixel &p)
    {
        if(x > width) return;
        if(y > height) return;
        pixels[y * width + x] = p;
    }
};

std::size_t read_file(const std::filesystem::path &path, std::vector<uint8_t> &out_data)
{
    std::ifstream is { path, std::ios::binary };
    if(!is)
    {
        std::println("could not open {}", path.string());
        return 0;
    }
    auto length { std::filesystem::file_size(path) };
    out_data.resize(length);
    is.read(reinterpret_cast<char *>(out_data.data()), length);
    std::println("{}: \n  read {} bytes", path.string(), out_data.size());
    return out_data.size();
}

bool open_webp(const std::filesystem::path &path, WebPImage &out_image)
{
    if(read_file(path, out_image.data) == 0) return false;

    const auto Success = WebPGetInfo(
        out_image.data.data(),
        out_image.data.size(),
        &out_image.width,
        &out_image.height
    );

    if(Success)
    {
        std::println(
            "WebPGetInfo: \n  file={}\n  width={}, height={}",
            path.string(),
            out_image.width,
            out_image.height
        );
        out_image.pixels.resize(
            static_cast<size_t>(out_image.width) *
            static_cast<size_t>(out_image.height)
        );

        const auto Success = WebPDecodeRGBInto(
            out_image.data.data(),
            out_image.data.size(),
            reinterpret_cast<uint8_t *>(out_image.pixels.data()),
            out_image.pixels.size() * sizeof(Pixel),
            sizeof(Pixel) * out_image.width
        );
        if(!Success)
        {
            std::println("WebPDecodeRGBInto: failed to decode image");
            return false;
        }

        /*
        WebPDecoderConfig config;
        WebPInitDecoderConfig(&config);
        if(const auto Result = WebPGetFeatures(out_image.data.data(), out_image.data.size(), &config.input) != VP8_STATUS_OK)
        {
            std::println("WebPGetFeatures: failed with error code {}", Result);
            return false;
        }
        else
        {
            std::println(
                "WebPGetFeatures:\n  file={}\n  format={}\n  width={}\n  height={}",
                path.string(),
                config.input.format,
                config.input.width,
                config.input.height
            );
        }
        // Specify the desired output colorspace:
        config.output.colorspace = MODE_RGB;
        // Have config.output point to an external buffer:
        config.output.u.RGBA.rgba = reinterpret_cast<uint8_t *>(out_image.pixels.data());
        config.output.u.RGBA.stride = sizeof(Pixel) * out_image.width;
        config.output.u.RGBA.size = out_image.pixels.size() * sizeof(Pixel);
        config.output.is_external_memory = 1;
        if(const auto Result = WebPDecode(out_image.data.data(), out_image.data.size(), &config) != VP8_STATUS_OK)
        {
            std::println("WebPDecode: failed with error code {}", Result);
            return false;
        }

        WebPDecoderOptions decoder_options;
        */


        return true;
    }
    else
    {
        std::println("WebPGetInfo: failed to parse {}", path.string());
        return false;
    }
}

int main(int argc, char *argv[])
{
    int opt;
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("image", po::value<std::string>(), "input image")
        ("mask", po::value<std::string>(), "mask image")
        ("output", po::value<std::string>(), "output image")
        ("alpha", po::value<float>(), "alpha value")
        ("r", po::value<int>(), "overlay color red")
        ("g", po::value<int>(), "overlay color green")
        ("b", po::value<int>(), "overlay color blue")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    WebPImage image, mask;
    float alpha;
    int overlay_r, overlay_g, overlay_b;

    if(!open_webp(vm["image"].as<std::string>(), image))
        return -1;
    if(!open_webp(vm["mask"].as<std::string>(), mask))
        return -1;

    alpha = vm["alpha"].as<float>();
    overlay_r = vm["r"].as<int>();
    overlay_g = vm["g"].as<int>();
    overlay_b = vm["b"].as<int>();

    std::println(
        "alpha={}\noverlay_color=[{},{},{}]",
        alpha,
        overlay_r,
        overlay_g,
        overlay_b
    );

    std::println("using equation c_original=(c_final-c_overlay*(1-alpha))/alpha to restore image colors");

    WebPImage recovered = image;

    const auto Eq = [](
        const uint8_t c_final,
        const float alpha,
        const uint8_t c_overlay) {
        const float value = (static_cast<float>(c_final) -
            static_cast<float>(c_overlay) * (1.0f - alpha)) / alpha;
        return std::clamp(value, 0.f, 255.f);
    };

    for(auto y = 0; y < image.height; ++y)
    {
        for(auto x = 0; x < image.width; ++x)
        {
            Pixel c_mask = mask.get_pixel(x, y);
            if(c_mask == BLACK)
                continue;
            Pixel c_f = image.get_pixel(x, y);
            Pixel c_original;
            c_original.r = Eq(c_f.r, alpha, overlay_r);
            c_original.g = Eq(c_f.g, alpha, overlay_g);
            c_original.b = Eq(c_f.b, alpha, overlay_b);
            recovered.set_pixel(x, y, c_original);
        }
    }

    std::filesystem::path output = vm["output"].as<std::string>();

    uint8_t *out_data = nullptr;
    const auto out_size = WebPEncodeLosslessRGB(
        reinterpret_cast<const uint8_t *>(recovered.pixels.data()),
        recovered.width,
        recovered.height,
        recovered.width * sizeof(Pixel),
        &out_data
    );

    if(!out_data)
    {
        std::println("WebPEncodeLosslessRGB: failed to encode image");
        return -1;
    }

    std::println("Saving to {}", output.string());
    std::ofstream of { output, std::ios::binary | std::ios::trunc };
    if(!of)
    {
        std::println("failed to open file");
        return -1;
    }
    of.write(reinterpret_cast<const char *>(out_data), out_size);
}
