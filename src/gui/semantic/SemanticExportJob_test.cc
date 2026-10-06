#include "core/semantic/SemanticTestFixture.h"
#include "gui/semantic/SemanticExportJob.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <sstream>
using SemanticTest::Fixture;
namespace {
QByteArray bytes(const QString& path) { QFile file(path); REQUIRE(file.open(QIODevice::ReadOnly)); return file.readAll(); }
void write(const QString& path, QByteArray data) { QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(data) == data.size()); }
BoundingBox stlBounds(const QByteArray& data) {
  std::istringstream input(data.toStdString()); std::string word; BoundingBox bounds;
  while (input >> word) if (word == "vertex") { double x,y,z; input >> x >> y >> z; REQUIRE(input.good()); bounds.extend(Vector3d(x,y,z)); }
  return bounds;
}
}
TEST_CASE("Semantic target exports select exact local placed and definition geometry atomically", "[semantic-export]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto info=createExportInfo(FileFormat::ASCII_STL,fileformat::info(FileFormat::ASCII_STL),fixture.root.string(),nullptr,{});
  auto right=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair/right"},"result",fixture.directory.string());
  for (const auto space : {Semantic::ExportSpace::Local,Semantic::ExportSpace::AsPlaced}) {
    auto destination=QString::fromStdString((fixture.directory/"right-\xc3\xa9.stl").string()); write(destination,"old destination");
    auto staged=Semantic::stageExport(right,space,destination,info); INFO(staged.error.toStdString()); REQUIRE(staged.valid()); REQUIRE(bytes(destination) == "old destination");
    auto bounds=stlBounds(bytes(staged.file->path)); REQUIRE(bounds.min().isApprox(space==Semantic::ExportSpace::Local?Vector3d(0,0,0):Vector3d(60,0,0)));
    REQUIRE(bounds.max().isApprox(space==Semantic::ExportSpace::Local?Vector3d(45,10,3):Vector3d(105,10,3)));
    REQUIRE(Semantic::commitExport(staged,[]{return true;})); REQUIRE(bytes(destination) == bytes(staged.file->path));
  }
  auto definition=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Definition,"bracket"},"result",fixture.directory.string());
  auto staged=Semantic::stageExport(definition,Semantic::ExportSpace::Local,QString::fromStdString((fixture.directory/"definition.stl").string()),info); REQUIRE(staged.valid()); REQUIRE(stlBounds(bytes(staged.file->path)).max().isApprox(Vector3d(30,10,3)));
}
TEST_CASE("Semantic failed stale cancelled and incompatible exports preserve existing destinations", "[semantic-export]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto target=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair/right"},"result",fixture.directory.string());
  auto destination=QString::fromStdString((fixture.directory/"existing.stl").string()); write(destination,"keep");
  auto info=createExportInfo(FileFormat::ASCII_STL,fileformat::info(FileFormat::ASCII_STL),fixture.root.string(),nullptr,{});
  auto staged=Semantic::stageExport(target,Semantic::ExportSpace::Local,destination,info); REQUIRE(staged.valid());
  QString error; REQUIRE_FALSE(Semantic::commitExport(staged,[]{return false;},&error)); REQUIRE(bytes(destination) == "keep");
  int validations=0; REQUIRE_FALSE(Semantic::commitExport(staged,[&]{return ++validations == 1;},&error)); REQUIRE(validations == 2); REQUIRE(bytes(destination) == "keep");
  auto cancelled=Semantic::stageExport(target,Semantic::ExportSpace::Local,destination,info,[]{return true;}); REQUIRE_FALSE(cancelled.valid()); REQUIRE(bytes(destination) == "keep");
  auto svg=createExportInfo(FileFormat::SVG,fileformat::info(FileFormat::SVG),fixture.root.string(),nullptr,{});
  REQUIRE_FALSE(Semantic::stageExport(target,Semantic::ExportSpace::Local,destination,svg).valid()); REQUIRE(bytes(destination) == "keep");
  REQUIRE_FALSE(Semantic::stageExport(target,Semantic::ExportSpace::Local,QString::fromStdString((fixture.directory/"absent-dir/file.stl").string()),info).valid());
  auto path=staged.file->path; staged={}; REQUIRE_FALSE(QFileInfo::exists(path));
}
TEST_CASE("Semantic targeted mesh formats reuse native exporters and complete current target ownership", "[semantic-export]") {
  Fixture fixture; fixture.evaluate(SemanticTest::pair);
  auto target=Semantic::prepareTarget(fixture.scene,{Semantic::TargetRole::Instance,"/pair/right"},"result",fixture.directory.string());
  for (auto format : {FileFormat::BINARY_STL,FileFormat::OBJ,FileFormat::OFF,FileFormat::_3MF}) {
    auto info=createExportInfo(format,fileformat::info(format),fixture.root.string(),nullptr,{});
    auto destination=QString::fromStdString((fixture.directory/("target."+fileformat::toSuffix(format))).string());
    auto staged=Semantic::stageExport(target,Semantic::ExportSpace::Local,destination,info); INFO(staged.error.toStdString()); REQUIRE(staged.valid()); REQUIRE(QFileInfo(staged.file->path).size() > 50);
    REQUIRE(Semantic::commitExport(staged,[]{return true;}));
  }
}
