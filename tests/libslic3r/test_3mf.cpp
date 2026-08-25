
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/filesystem/operations.hpp>

#include <catch2/catch_tostring.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <type_traits> // for std::enable_if_t
#include <typeinfo>    // for typeid

namespace Catch {
    template <typename T>
    struct is_eigen_matrix : std::is_base_of<Eigen::MatrixBase<T>, T> {};

    template <typename T>
    struct StringMaker<T, std::enable_if_t<is_eigen_matrix<T>::value>> {
        static std::string convert(const T& eigen_obj) {
            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            ss << "Matrix<" << typeid(eigen_obj).name() << "> = \n";
            ss << eigen_obj.format(fmt);
            return ss.str();
        }
    };
    
    // We must manually specialize for Eigen::Transform as it doesn't derive from MatrixBase.
    // It's defined as: Eigen::Transform<Scalar, Dim, Mode, Options>
    template <typename Scalar, int Dim, int Mode, int Options>
    struct StringMaker<Eigen::Transform<Scalar, Dim, Mode, Options>> {
        static std::string convert(const Eigen::Transform<Scalar, Dim, Mode, Options>& trafo) {
            // We print the underlying matrix 
            const auto& matrix = trafo.matrix();

            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            
            ss << "Transform<Mode=" << Mode << ", Dim=" << Dim << "> = \n"; 
            ss << matrix.format(fmt);
            return ss.str();
        }
    };
    
    // Quaternions also need an explicit specialization
    template <typename Scalar, int Options>
    struct StringMaker<Eigen::Quaternion<Scalar, Options>> {
        static std::string convert(const Eigen::Quaternion<Scalar, Options>& quat) {
            std::stringstream ss;
            ss << "Quaternion(w=" << quat.w() << ", x=" << quat.x() << ", y=" << quat.y() << ", z=" << quat.z() << ")";
            return ss.str();
        }
    };
} // end namespace Catch

#include <catch2/catch_all.hpp>

using namespace Slic3r;


SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
            std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
            DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

TEST_CASE("Mixed support paint survives a 3MF round trip", "[3mf][Mixed][Painting]")
{
    Model source;
    const std::string source_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    REQUIRE(load_stl(source_file.c_str(), &source));
    source.add_default_instances();

    ModelVolume *volume = source.objects.front()->volumes.front();
    TriangleSelector support_selector(volume->mesh());
    support_selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    support_selector.set_facet(1, EnforcerBlockerType::ENFORCER);
    REQUIRE(volume->supported_facets.set(support_selector));

    TriangleSelector channel_selector(volume->mesh());
    channel_selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    channel_selector.set_facet(1, EnforcerBlockerType::BLOCKER);
    REQUIRE(volume->mixed_support_facets.set(channel_selector));

    const boost::filesystem::path output =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mixed-support-paint-%%%%-%%%%.3mf");
    REQUIRE(store_3mf(output.string().c_str(), &source, nullptr, false));

    Model restored;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(load_3mf(output.string().c_str(), config, substitutions, &restored, false));
    boost::filesystem::remove(output);

    REQUIRE(restored.objects.size() == 1);
    REQUIRE(restored.objects.front()->volumes.size() == 1);
    const ModelVolume *restored_volume = restored.objects.front()->volumes.front();
    REQUIRE(restored_volume->supported_facets.equals(volume->supported_facets));
    REQUIRE(restored_volume->mixed_support_facets.equals(volume->mixed_support_facets));
}

TEST_CASE("Mixed support paint survives a Bambu-Orca 3MF round trip", "[3mf][bbs_3mf][Mixed][Painting]")
{
    Model source;
    const std::string source_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    REQUIRE(load_stl(source_file.c_str(), &source));
    source.add_default_instances();

    ModelVolume *volume = source.objects.front()->volumes.front();
    TriangleSelector support_selector(volume->mesh());
    support_selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    support_selector.set_facet(1, EnforcerBlockerType::ENFORCER);
    REQUIRE(volume->supported_facets.set(support_selector));

    TriangleSelector channel_selector(volume->mesh());
    channel_selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    channel_selector.set_facet(1, EnforcerBlockerType::BLOCKER);
    REQUIRE(volume->mixed_support_facets.set(channel_selector));

    const boost::filesystem::path output =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mixed-support-paint-bbs-%%%%-%%%%.3mf");
    DynamicPrintConfig source_config;
    StoreParams store_params;
    const std::string output_string = output.string();
    std::set<std::pair<int, int>> object_instances{{0, 0}};
    PlateData source_plate(0, object_instances, false);
    store_params.path = output_string.c_str();
    store_params.model = &source;
    store_params.plate_data_list = {&source_plate};
    store_params.config = &source_config;
    store_params.strategy = SaveStrategy::Silence | SaveStrategy::Zip64 | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store_params));

    Model restored;
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    bool is_bambu = false;
    bool is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(output_string.c_str(), &restored_config, &substitutions, &restored,
                         &plates, &presets, &is_bambu, &is_orca, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances));
    release_PlateData_list(plates);
    for (Preset *preset : presets)
        delete preset;
    boost::filesystem::remove(output);

    REQUIRE(restored.objects.size() == 1);
    REQUIRE(restored.objects.front()->volumes.size() == 1);
    const ModelVolume *restored_volume = restored.objects.front()->volumes.front();
    REQUIRE(restored_volume->supported_facets.equals(volume->supported_facets));
    REQUIRE(restored_volume->mixed_support_facets.equals(volume->mixed_support_facets));
}

SCENARIO("2D convex hull of sinking object", "[3mf][.]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects[0];
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled (and sinking? how is it sinking? the rotation? does it matter if it's sinking?)
            ModelInstance* instance = object->instances[0];
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
	    auto trafo = instance->get_transformation().get_matrix();

	    // This matrix is the same exact matrix as the Prusaslicer test
	    CAPTURE(trafo);
            Polygon hull_2d = object->convex_hull_2d(trafo);

	    // But we get different hull_2d.points here (and somehow decimal numbers despite being int64_t values, but that's probabaly printing configuration somewhere -- Prusaslicer's prints out with newlines between the X&Y and not one between coordinates, which is about the worse possible output).
	    // I think it's something to do with PrusaSlicer ignoring everything under the Z plane, which makes sense from the results.
	    // See the comments added to ModelObject::convex_hull_2d for more information.

            // verify result
            Points result = {
                { -91501496, -15914144 },
                { 91501496, -15914144 },
                { 91501496, 4243 },
                { 78229680, 4246883 },
                { 56898100, 4246883 },
                { -85501496, 4242641 },
                { -91501496, 4243 }
            };

            THEN("2D convex hull should match with reference") {
                // Allow 1um error due to floating point rounding.
                bool res = hull_2d.points.size() == result.size();
                if (res) {
                    for (size_t i = 0; i < result.size(); ++ i) {
                        const Point &p1 = result[i];
                        const Point &p2 = hull_2d.points[i];
                        CHECK(std::abs(p1.x() - p2.x()) <= 1);
                        CHECK(std::abs(p1.y() - p2.y()) <= 1);
                    }
                }

                CAPTURE(hull_2d.points);
                REQUIRE(res);
            }
        }
    }
}
