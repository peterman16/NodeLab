#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/Tiff.h"

namespace fs = std::filesystem;

namespace {

// A flat-coloured opaque PNG in a fresh temp folder.
std::string writeSource(const fs::path& dir, const std::string& name, float v, int w = 40, int h = 20) {
    Image img(w, h);
    for (size_t i = 0; i < img.pixelCount(); ++i) {
        float* p = img.pixel(i);
        p[0] = v, p[1] = v * 0.5f, p[2] = 0.25f, p[3] = 1.0f;
    }
    std::string err;
    const std::string path = pathToU8(dir / name);
    REQUIRE(saveImage(path, img, err));
    return path;
}

}  // namespace

TEST_CASE("export settings round-trip and clamp") {
    ExportSettings s;
    s.format = ExportSettings::JPEG;
    s.jpegQuality = 80;
    s.sizeMode = ExportSettings::LongEdge;
    s.longEdge = 1000;
    s.nameTemplate = "{name}_x";
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.format == ExportSettings::JPEG);
    CHECK(t.jpegQuality == 80);
    CHECK(t.longEdge == 1000);
    CHECK(t.nameTemplate == "{name}_x");
    // Settings saved before templates had a suffix.
    ExportSettings old;
    old.fromJson({{"suffix", "_web"}});
    CHECK(old.nameTemplate == "{name}_web");
    t.fromJson({{"jpegQuality", 500}, {"percent", -3}});
    CHECK(t.jpegQuality == 100);
    CHECK(t.percent == 1);
}

TEST_CASE("resizeForExport only shrinks") {
    auto img = std::make_shared<const Image>(400, 200);
    ExportSettings s;
    CHECK(resizeForExport(img, s) == img);
    s.sizeMode = ExportSettings::LongEdge;
    s.longEdge = 100;
    auto r = resizeForExport(img, s);
    CHECK(r->w == 100);
    CHECK(r->h == 50);
    s.longEdge = 4000;
    CHECK(resizeForExport(img, s)->w == 400);
    s.sizeMode = ExportSettings::Percent;
    s.percent = 25;
    CHECK(resizeForExport(img, s)->w == 100);
}

TEST_CASE("Output Sharpening is off by default, and stronger for paper and higher amounts") {
    auto img = std::make_shared<Image>(64, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x) {
            float* p = img->pixel(size_t(y) * 64 + x);
            p[0] = p[1] = p[2] = x < 32 ? 0.2f : 0.6f;
            p[3] = 1.0f;
        }
    ImagePtr src = img;
    ExportSettings s;
    CHECK(sharpenForExport(src, s, true) == src);
    // Overshoot on the dark side of the edge.
    auto dip = [&](int target, int amount) {
        s.sharpenFor = target, s.sharpenAmount = amount;
        return 0.2f - sharpenForExport(src, s, true)->pixel(4 * 64 + 31)[0];
    };
    CHECK(dip(ExportSettings::Screen, 0) > 0.0f);
    CHECK(dip(ExportSettings::Screen, 2) > dip(ExportSettings::Screen, 0));
    CHECK(dip(ExportSettings::Matte, 1) > dip(ExportSettings::Screen, 1));
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.sharpenFor == ExportSettings::Matte);
    CHECK(t.sharpenAmount == 1);
}

TEST_CASE("batchOutputPath never overwrites the source") {
    ExportSettings s;
    s.nameTemplate = "{name}";
    const fs::path dir = fs::temp_directory_path() / "nodelab_batch_name";
    fs::create_directories(dir);
    const std::string src = writeSource(dir, "a.png", 0.5f);
    const std::string same = batchOutputPath(src, pathToU8(dir), s);
    CHECK(u8ToPath(same).filename() == "a_edit.png");
    s.format = ExportSettings::JPEG;
    CHECK(u8ToPath(batchOutputPath(src, pathToU8(dir), s)).filename() == "a.jpg");
    fs::remove_all(dir);
}

TEST_CASE("Batch exports keep the source's name but never replace an original") {
    ExportSettings s;
    CHECK(s.nameTemplate == "{name}");
    // The old default, saved in older projects and presets, means the new one.
    ExportSettings old;
    old.fromJson({{"nameTemplate", "{name}_edit"}});
    CHECK(old.nameTemplate == "{name}");

    const fs::path dir = fs::temp_directory_path() / "nodelab_batch_collide";
    fs::remove_all(dir);
    fs::create_directories(dir / "out");
    // A RAW+JPEG pair: exporting the "RAW" (a.png here) as JPEG into its own folder must not
    // replace the camera's a.jpg, which isn't in the batch.
    const std::string raw = writeSource(dir, "a.png", 0.5f);
    std::ofstream(dir / "a.jpg") << "camera";
    s.format = ExportSettings::JPEG;
    auto outs = batchOutputPaths({{raw}}, pathToU8(dir), s);
    CHECK(u8ToPath(outs[0]).filename() == "a (2).jpg");
    // Both of the pair in one batch: neither export replaces the other's source.
    const std::string jpg = pathToU8(dir / "a.jpg");
    outs = batchOutputPaths({{raw}, {jpg}}, pathToU8(dir), s);
    CHECK(u8ToPath(outs[0]).filename() == "a (2).jpg");
    CHECK(u8ToPath(outs[1]).filename() == "a_edit.jpg");

    // In a separate folder an earlier export is replaced, but a Library photo isn't.
    std::ofstream(dir / "out" / "a.jpg") << "earlier export";
    outs = batchOutputPaths({{raw}}, pathToU8(dir / "out"), s);
    CHECK(u8ToPath(outs[0]).filename() == "a.jpg");
    std::ofstream(dir / "out" / "a.jpg.nlproj") << "{}";
    outs = batchOutputPaths({{raw}}, pathToU8(dir / "out"), s);
    CHECK(u8ToPath(outs[0]).filename() == "a (2).jpg");
    fs::remove_all(dir);
}

TEST_CASE("Exporter runs a batch through the graph on a background thread") {
    const fs::path dir = fs::temp_directory_path() / "nodelab_batch_run";
    fs::remove_all(dir);
    fs::create_directories(dir / "out");
    const std::string a = writeSource(dir, "a.png", 1.0f, 40, 20);
    const std::string b = writeSource(dir, "b.png", 0.0f, 30, 30);

    Graph g;
    Node* in = g.addNode("io.image_input");
    Node* inv = g.addNode("color.invert");
    Node* out = g.addNode("io.output");
    g.connect(in->id, 0, inv->id, 0);
    g.connect(inv->id, 0, out->id, 0);

    ExportSettings s;  // PNG, "{name}"
    std::vector<ExportItem> items;
    for (const std::string& src : {a, b, pathToU8(dir / "missing.png")})
        items.push_back({src, batchOutputPath(src, pathToU8(dir / "out"), s)});
    Exporter ex;
    ex.start(g.toJson(), items, in->id, s);
    ex.wait();
    CHECK_FALSE(ex.busy());
    const Exporter::Progress pr = ex.progress();
    CHECK(pr.done == 3);
    CHECK(pr.failed == 1);  // the missing source
    CHECK(ex.takeLog().size() == 3);

    std::string err;
    auto ra = loadImage(pathToU8(dir / "out" / "a.png"), err);
    auto rb = loadImage(pathToU8(dir / "out" / "b.png"), err);
    REQUIRE(ra);
    REQUIRE(rb);
    // Each result has its own source's size, and was inverted.
    CHECK(ra->w == 40);
    CHECK(rb->h == 30);
    CHECK(ra->pixel(0)[0] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(rb->pixel(0)[0] == doctest::Approx(1.0f).epsilon(0.01));
    fs::remove_all(dir);
}

TEST_CASE("Exporter can be cancelled") {
    Graph g;
    g.addNode("io.output");
    Exporter ex;
    ex.cancel();
    std::vector<ExportItem> items(50, ExportItem{"", pathToU8(fs::temp_directory_path() / "nodelab_never.png")});
    ex.start(g.toJson(), items, 0, ExportSettings{});
    ex.cancel();
    ex.wait();
    CHECK(ex.progress().done < 50);
}

TEST_CASE("Filename templates expand tokens and make safe names") {
    const fs::path dir = fs::temp_directory_path() / "nodelab_name_template";
    fs::remove_all(dir);
    fs::create_directories(dir / "Trip");
    const std::string src = writeSource(dir / "Trip", "IMG_7.png", 0.5f);
    CHECK(expandNameTemplate("{name}_edit", {src}) == "IMG_7_edit");
    CHECK(expandNameTemplate("{NAME}-{seq}", {src, 12}) == "IMG_7-12");
    CHECK(expandNameTemplate("{folder}_{seq:3}", {src, 7}) == "Trip_007");
    CHECK(expandNameTemplate("{name} {copy}", {src, 1, 2}) == "IMG_7 Copy 2");
    CHECK(expandNameTemplate("{name} {copy}", {src, 1, 0}) == "IMG_7");  // trailing space trimmed
    // A PNG has no EXIF: the date is the file's, the camera is empty.
    const std::string date = expandNameTemplate("{date}", {src});
    REQUIRE(date.size() == 10);
    CHECK(date[4] == '-');
    CHECK(expandNameTemplate("{year}", {src}) == date.substr(0, 4));
    CHECK(expandNameTemplate("{name}{camera}", {src}) == "IMG_7");
    // Unknown tokens stay as written; characters Windows forbids become _.
    CHECK(expandNameTemplate("{name}{nope}", {src}) == "IMG_7{nope}");
    CHECK(expandNameTemplate("a/b:c?{name}", {src}) == "a_b_c_IMG_7");
    // Nothing left: the source's name.
    CHECK(expandNameTemplate("{camera}", {src}) == "IMG_7");
    CHECK(expandNameTemplate("{name}", {""}) == "export");

    // A batch whose template names two files the same numbers the second.
    ExportSettings s;
    s.nameTemplate = "photo";
    const std::string other = writeSource(dir, "b.png", 0.2f);
    const auto outs = batchOutputPaths({{src}, {other}}, pathToU8(dir / "out"), s);
    REQUIRE(outs.size() == 2);
    CHECK(u8ToPath(outs[0]).filename() == "photo.png");
    CHECK(u8ToPath(outs[1]).filename() == "photo (2).png");
    s.nameTemplate = "{seq:2}_{name}";
    const auto seq = batchOutputPaths({{src}, {other}}, pathToU8(dir / "out"), s);
    CHECK(u8ToPath(seq[1]).filename() == "02_b.png");
    fs::remove_all(dir);
}

TEST_CASE("Export presets: built-ins are distinct and recognised") {
    const auto& presets = builtInExportPresets();
    REQUIRE(presets.size() >= 4);
    for (size_t i = 0; i < presets.size(); ++i)
        for (size_t j = i + 1; j < presets.size(); ++j) {
            CHECK(presets[i].name != presets[j].name);
            CHECK_FALSE(presets[i].settings.sameOutput(presets[j].settings));
        }
    ExportSettings s = presets[1].settings;
    s.fileOutputs = !s.fileOutputs;  // not part of a preset
    CHECK(s.sameOutput(presets[1].settings));
    s.nameTemplate = "{seq}";
    CHECK_FALSE(s.sameOutput(presets[1].settings));
}

TEST_CASE("Photo info comes from EXIF, and names exports") {
    // An EXIF block as a camera writes it, put into a JPEG's APP1 segment.
    tiff::Ifd ifd0, ex;
    ifd0.ascii(0x010F, "Canon");
    ifd0.ascii(0x0110, "Canon EOS R6");
    ifd0.ascii(0x0132, "2024:05:06 07:08:09");
    ex.ascii(0x9003, "2023:01:02 03:04:05");
    ex.rational(0x829A, 1, 250);
    ex.rational(0x829D, 28, 10);
    ex.shorts(0x8827, {800});
    ex.rational(0x920A, 50, 1);
    ex.ascii(0xA434, "RF50mm F1.8 STM");
    std::vector<uint8_t> block = tiff::header();
    const uint32_t exOff = ex.write(block);
    ifd0.longs(0x8769, {exOff});
    tiff::set32(block, 4, ifd0.write(block));

    exif::PhotoInfo info;
    REQUIRE(exif::infoFromTiff(block.data(), block.size(), info));
    CHECK(info.model == "Canon EOS R6");
    CHECK(info.captureTime == "2023:01:02 03:04:05");  // DateTimeOriginal, not DateTime
    CHECK(info.exposureTime == doctest::Approx(1.0 / 250));
    CHECK(info.fNumber == doctest::Approx(2.8));
    CHECK(info.iso == 800);
    CHECK(info.lens == "RF50mm F1.8 STM");
    // Damaged blocks read nothing, without reading past the end.
    for (size_t cut = 0; cut < block.size(); cut += 7) {
        exif::PhotoInfo part;
        exif::infoFromTiff(block.data(), cut, part);
    }

    const fs::path dir = fs::temp_directory_path() / "nodelab_exif_names";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string png = writeSource(dir, "x.png", 0.5f);
    Image img(8, 8);
    std::string err;
    const std::string jpgPath = pathToU8(dir / "shot.jpg");
    REQUIRE(saveImage(jpgPath, img, err));
    std::vector<char> bytes;
    {
        std::ifstream f(u8ToPath(jpgPath), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::vector<char> app1 = {char(0xFF), char(0xE1), 0, 0, 'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), block.begin(), block.end());
    app1[2] = char((app1.size() - 2) >> 8), app1[3] = char((app1.size() - 2) & 0xFF);
    bytes.insert(bytes.begin() + 2, app1.begin(), app1.end());
    std::ofstream(u8ToPath(jpgPath), std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));

    REQUIRE(exif::readInfo(jpgPath, info));
    CHECK(info.make == "Canon");
    CHECK(expandNameTemplate("{date}_{time}_{camera}", {jpgPath}) == "2023-01-02_030405_Canon EOS R6");
    CHECK(expandNameTemplate("{iso} {focal} {aperture} {shutter}", {jpgPath}) == "800 50mm f2.8 1-250s");
    CHECK_FALSE(exif::readInfo(png, info));
    fs::remove_all(dir);
}
