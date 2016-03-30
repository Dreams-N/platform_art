package aurelienribon.bodyeditor;

import com.badlogic.gdx.files.FileHandle;
import com.badlogic.gdx.utils.JsonReader;
import com.badlogic.gdx.utils.JsonValue;

import org.jbox2d.collision.shapes.CircleShape;
import org.jbox2d.collision.shapes.PolygonShape;
import org.jbox2d.common.Vec2;
import org.jbox2d.dynamics.Body;
import org.jbox2d.dynamics.FixtureDef;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Loads the collision fixtures defined with the Physics Body Editor
 * application. You only need to give it a body and the corresponding fixture
 * name, and it will attach these fixtures to your body.
 *
 * @author Aurelien Ribon | http://www.aurelienribon.com
 */
public class BodyEditorLoader {

    // Model
    private final Model model;

    // Reusable stuff
    private final List<Vec2> vectorPool = new ArrayList<Vec2>();
    private final PolygonShape polygonShape = new PolygonShape();
    private final CircleShape circleShape = new CircleShape();
    private final Vec2 vec = new Vec2();

    // -------------------------------------------------------------------------
    // Ctors
    // -------------------------------------------------------------------------
    public BodyEditorLoader(FileHandle file) {
        if (file == null) throw new NullPointerException("file is null");
        model = readJson(file.readString());
    }

    public BodyEditorLoader(String str) {
        if (str == null) throw new NullPointerException("str is null");
        model = readJson(str);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    /**
     * Creates and applies the fixtures defined in the editor. The name
     * parameter is used to retrieve the right fixture from the loaded file.
     * <br/><br/>
     * <p/>
     * The body reference point (the red cross in the tool) is by default
     * located at the bottom left corner of the image. This reference point
     * will be put right over the BodyDef position point. Therefore, you should
     * place this reference point carefully to let you place your body in your
     * world easily with its BodyDef.position point. Note that to draw an image
     * at the position of your body, you will need to know this reference point
     * (see {@link #getOrigin(java.lang.String, float)}.
     * <br/><br/>
     * <p/>
     * Also, saved shapes are normalized. As shown in the tool, the width of
     * the image is considered to be always 1 meter. Thus, you need to provide
     * a scale factor so the polygons get resized according to your needs (not
     * every body is 1 meter large in your game, I guess).
     *
     * @param body  The Box2d body you want to attach the fixture to.
     * @param name  The name of the fixture you want to load.
     * @param fd    The fixture parameters to apply to the created body fixture.
     * @param scale The desired scale of the body. The default width is 1.
     */
    public void attachFixture(Body body, String name, FixtureDef fd, float scale) {
        RigidBodyModel rbModel = model.rigidBodies.get(name);
        if (rbModel == null) {
            throw new RuntimeException("Name '" + name + "' was not found.");
        }
        Vec2 origin = vec.set(rbModel.origin).mul(scale);

        for (PolygonModel polygon : rbModel.polygons) {
            Vec2[] vertices = polygon.buffer;

            for (int ii = 0, nn = vertices.length; ii < nn; ii++) {
                vertices[ii] = newVec().set(polygon.vertices.get(ii)).mul(scale);
                vertices[ii].sub(origin);
            }

            polygonShape.set(vertices, vertices.length);
            fd.shape = polygonShape;
            body.createFixture(fd);

            for (Vec2 vertex : vertices) {
                free(vertex);
            }
        }

        for (CircleModel circle : rbModel.circles) {
            Vec2 center = newVec().set(circle.center).mul(scale);
            float radius = circle.radius * scale;

            circleShape.m_p.set(center);
            circleShape.setRadius(radius);
            fd.shape = circleShape;
            body.createFixture(fd);

            free(center);
        }
    }

    /**
     * Gets the image path attached to the given name.
     */
    public String getImagePath(String name) {
        RigidBodyModel rbModel = model.rigidBodies.get(name);
        if (rbModel == null) throw new RuntimeException("Name '" + name + "' was not found.");

        return rbModel.imagePath;
    }

    /**
     * Gets the origin point attached to the given name. Since the point is
     * normalized in [0,1] coordinates, it needs to be scaled to your body
     * size. Warning: this method returns the same Vec2 object each time, so
     * copy it if you need it for later use.
     */
    public Vec2 getOrigin(String name, float scale) {
        RigidBodyModel rbModel = model.rigidBodies.get(name);
        if (rbModel == null) throw new RuntimeException("Name '" + name + "' was not found.");

        return vec.set(rbModel.origin).mul(scale);
    }

    /**
     * <b>For advanced users only.</b> Lets you access the internal model of
     * this loader and modify it. Be aware that any modification is permanent
     * and that you should really know what you are doing.
     */
    public Model getInternalModel() {
        return model;
    }

    // -------------------------------------------------------------------------
    // Json Models
    // -------------------------------------------------------------------------

    public static class Model {
        public final Map<String, RigidBodyModel> rigidBodies = new HashMap<String, RigidBodyModel>();
    }

    public static class RigidBodyModel {
        public String name;
        public String imagePath;
        public final Vec2 origin = new Vec2();
        public final List<PolygonModel> polygons = new ArrayList<PolygonModel>();
        public final List<CircleModel> circles = new ArrayList<CircleModel>();
    }

    public static class PolygonModel {
        public final List<Vec2> vertices = new ArrayList<Vec2>();
        private Vec2[] buffer; // used to avoid allocation in attachFixture()
    }

    public static class CircleModel {
        public final Vec2 center = new Vec2();
        public float radius;
    }

    // -------------------------------------------------------------------------
    // Json reading process
    // -------------------------------------------------------------------------

    private Model readJson(String str) {
        Model m = new Model();

        JsonValue map = new JsonReader().parse(str);

        JsonValue bodyElem = map.getChild("rigidBodies");
        for (; bodyElem != null; bodyElem = bodyElem.next()) {
            RigidBodyModel rbModel = readRigidBody(bodyElem);
            m.rigidBodies.put(rbModel.name, rbModel);
        }

        return m;
    }

    private RigidBodyModel readRigidBody(JsonValue bodyElem) {
        RigidBodyModel rbModel = new RigidBodyModel();
        rbModel.name = bodyElem.getString("name");
        rbModel.imagePath = bodyElem.getString("imagePath");

        JsonValue originElem = bodyElem.get("origin");
        rbModel.origin.x = originElem.getFloat("x");
        rbModel.origin.y = originElem.getFloat("y");

        // polygons
        JsonValue polygonsElem = bodyElem.getChild("polygons");
        for (; polygonsElem != null; polygonsElem = polygonsElem.next()) {

            PolygonModel polygon = new PolygonModel();
            rbModel.polygons.add(polygon);

            JsonValue vertexElem = polygonsElem.child();
            for (; vertexElem != null; vertexElem = vertexElem.next()) {
                float x = vertexElem.getFloat("x");
                float y = vertexElem.getFloat("y");
                polygon.vertices.add(new Vec2(x, y));
            }

            polygon.buffer = new Vec2[polygon.vertices.size()];

        }

        // circles
        JsonValue circleElem = bodyElem.getChild("circles");

        for (; circleElem != null; circleElem = circleElem.next()) {
            CircleModel circle = new CircleModel();
            rbModel.circles.add(circle);

            circle.center.x = circleElem.getFloat("cx");
            circle.center.y = circleElem.getFloat("cy");
            circle.radius = circleElem.getFloat("r");
        }

        return rbModel;
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private Vec2 newVec() {
        return vectorPool.isEmpty() ? new Vec2() : vectorPool.remove(0);
    }

    private void free(Vec2 v) {
        vectorPool.add(v);
    }
}
