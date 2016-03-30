package com.intel.crtl.workload;

import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.Input;
import com.badlogic.gdx.graphics.FPSLogger;
import com.badlogic.gdx.graphics.GL20;
import com.badlogic.gdx.graphics.OrthographicCamera;
import com.badlogic.gdx.graphics.g2d.Sprite;
import com.badlogic.gdx.graphics.g2d.SpriteBatch;
import com.badlogic.gdx.graphics.g2d.TextureAtlas;
import com.badlogic.gdx.math.MathUtils;
import com.badlogic.gdx.math.Vector2;
import com.badlogic.gdx.physics.box2d.Body;
import com.badlogic.gdx.physics.box2d.BodyDef;
import com.badlogic.gdx.physics.box2d.BodyDef.BodyType;
import com.badlogic.gdx.physics.box2d.Box2DDebugRenderer;
import com.badlogic.gdx.physics.box2d.Contact;
import com.badlogic.gdx.physics.box2d.ContactImpulse;
import com.badlogic.gdx.physics.box2d.ContactListener;
import com.badlogic.gdx.physics.box2d.Manifold;
import com.badlogic.gdx.physics.box2d.World;
import com.badlogic.gdx.physics.box2d.joints.RevoluteJointDef;
import com.badlogic.gdx.physics.box2d.joints.WeldJointDef;

import java.util.Iterator;
import java.util.LinkedList;
import java.util.Vector;

import aurelienribon.bodyeditor.BodyEditorLoader;

import static com.intel.crtl.workload.IcyRocksGame.screenHeight;
import static com.intel.crtl.workload.IcyRocksGame.screenWidth;

/**
 * Created by wfeng on 2/24/16.
 */
public class IcyRocksScene extends BaseScene {
    private static final String TAG = "IcyRocks";
    private static final boolean DRAW_BOX2D_DEBUG = true;
    private static final float FACTOR = 32.0f; //100.0f;

    private Box2DDebugRenderer debugRenderer;
    private TextureAtlas atlas;
    private SpriteBatch batch;
    private World world;
    private OrthographicCamera box2dCam;
    private SpriteFactory factory;
    private OrthographicCamera camera;
    private IcyWorldSprite side;
    private IcyWorldSprite leftMount;
    private IcyWorldSprite rightMount;
    private IcyWorldSprite fan;
    private IcyWorldSprite snowman;
    private Ejector ejector;
    private Vector<IcyWorldSprite> rocks;
    private Vector<IcyWorldSprite> snows;

    private int nSnows = 0;
    private int nRocks = 0;
    private int nSnowStep = 5;
    private int nRockStep = 5;

    private boolean scored = false;
    private LinkedList<Sprite> scoreSprites = new LinkedList<Sprite>();

    private FPSLogger fpsLogger;

    public static final int SceneWidth = 1368;
    public static final int SceneHeight = 720;
    public static final int GroundWidth = 1368;
    public static final int GroundHeight = 20;
    public static final int MountainWidth = 600;
    public static final int MountainHeight = 384;
    public static final int SnowmanWidth = 90;
    public static final int EjectorLength = 75;
    public static final int EjectorHeight = MountainHeight;
    public static final int SnowmanHeight = 100;
    public static final int RockWidth = 50;
    public static final int RockHeight = 100;
    public static final int nRocksPerRow = 20;
    public static final int SnowWidth = 6;
    public static final int FanSize = 150;
    public static final int FanX = GroundWidth / 2;
    public static final int FanY = FanSize / 2 + 55;
    public static final float FanSpeed = 6 * MathUtils.radiansToDegrees;

    private final float fireInterval = 3f;
    private float timeToFire = fireInterval;

    private final float windInterval = 0.1f;
    private float timeToWind = windInterval;

    private Sprite intelLogo;

    public IcyRocksScene(IcyRocksGame game) {
        super(game);
        atlas = game.atlas;
        batch = game.batch;
        camera = game.camera;
        fpsLogger = new FPSLogger();
        rocks = new Vector<IcyWorldSprite>();
        snows = new Vector<IcyWorldSprite>();
        Gdx.input.setInputProcessor(this);
        initPhysics();
    }

    @Override
    public void render(float delta) {
        super.render(delta);
        Gdx.gl.glClearColor(122 / 255f, 228 / 255f, 1, 1);
        Gdx.gl.glClear(GL20.GL_COLOR_BUFFER_BIT);

        update(delta);
        draw();
        if (DRAW_BOX2D_DEBUG) {
            box2dCam.update();
            debugRenderer.render(world, box2dCam.combined);
        }
        fpsLogger.log();
    }

    @Override
    public boolean touchDown(int screenX, int screenY, int pointer, int button) {
        if (button == Input.Buttons.LEFT) return true;
        return false;
    }

    @Override
    public boolean touchUp(int screenX, int screenY, int pointer, int button) {
        if (button == Input.Buttons.LEFT) {
            addRocks(nRockStep);
            addSnows(nSnowStep);
            Gdx.app.log(TAG, "total " + nRocks + " rocks and " + nSnows + " snows");
            return true;
        }
        return true;
    }

    private void update(float dt) {
        world.step(dt, 10, 10);

        timeToFire -= dt;
        timeToWind -= dt;

        // fire stone
        if (timeToFire < 0) {
            ejector.fire(factory, world);
            timeToFire = fireInterval;
        }

        // apply wind
        if (timeToWind < 0) {
            applyWind();
            timeToWind = windInterval;
        }

        if (scored) {
            createScoreAnimation();
            scored = false;
        }

        if (addParticles) {
            IcyWorldSprite stone = ejector.getStone();
            Vector2 initVelocity = stone.getBody().getLinearVelocity().scl(.1f);
            particleRecords.add(new ParticleRecord(
                    createParticles(stone.getX(), stone.getY(), 6, 4, initVelocity), 1));
            addParticles = false;
        }

        ejector.update(world);

        for (Sprite sprite : scoreSprites) {
            sprite.setY(sprite.getY() + 300 * dt);
            if (sprite.getY() > SceneHeight - sprite.getRegionHeight()) {
                scoreSprites.remove(sprite);
            }
        }

        for (Iterator<ParticleRecord> i = particleRecords.iterator(); i.hasNext(); ) {
            ParticleRecord record = i.next();
            record.life -= dt;
            if (record.life < 0) {
                for (IcyWorldSprite sprite : record.sprites) {
                    world.destroyBody(sprite.getBody());
                }
                record.sprites.clear();
                i.remove();
            } else {
                for (IcyWorldSprite sprite : record.sprites) {
                    sprite.setAlpha(255 * record.life);
                }
            }
        }

    }

    private void draw() {
        camera.update();
        batch.setProjectionMatrix(camera.combined);
        batch.begin();
        batch.enableBlending();
        side.draw(batch);
        leftMount.draw(batch);
        rightMount.draw(batch);
        fan.draw(batch);
        snowman.draw(batch);
        ejector.draw(batch);

        for (IcyWorldSprite rock : rocks) {
            rock.draw(batch);
        }
        for (IcyWorldSprite snow : snows) {
            snow.draw(batch);
        }

        for (Sprite sprite : scoreSprites) {
            sprite.draw(batch);
        }
        for (ParticleRecord record : particleRecords) {
            for (IcyWorldSprite sprite : record.sprites) {
                sprite.draw(batch);
            }
        }

        intelLogo.draw(batch);
        batch.end();
    }

    private void initPhysics() {
        world = new World(new Vector2(0, -9.8f), true);
        BodyEditorLoader loader = new BodyEditorLoader(Gdx.files.internal("snowbodies.json"));
        factory = new SpriteFactory(world, loader, atlas, FACTOR);
        debugRenderer = new Box2DDebugRenderer();
        box2dCam = new OrthographicCamera(screenWidth / FACTOR, screenHeight / FACTOR);
        box2dCam.position.set(screenWidth / (2 * FACTOR), screenHeight / (2 * FACTOR), 0);
        createWorldBox();
        leftMount = factory.create("leftM",
                new Vector2(MountainWidth / 2, MountainHeight / 2),
                BodyType.StaticBody, 0f, .2f, .8f,
                SpriteFactory.SpriteShapes.Custom);

        rightMount = factory.create("rightM",
                new Vector2(SceneWidth - MountainWidth / 2, MountainHeight / 2),
                BodyType.StaticBody, 0f, .2f, .8f,
                SpriteFactory.SpriteShapes.Custom);
        fan = factory.create("fan",
                new Vector2(FanX, FanY), BodyType.DynamicBody, 1.0f, .2f, .8f,
                SpriteFactory.SpriteShapes.Custom);
        createFanJoint();

        snowman = factory.create("snowman",
                new Vector2(SceneWidth - SnowmanWidth / 2, MountainHeight + SnowmanHeight / 2),
                BodyType.DynamicBody, 1.0f, .2f, .8f,
                SpriteFactory.SpriteShapes.Custom);
        // Is this a fast moving body that should be prevented from tunneling
        // through other moving bodies? Note that all bodies are prevented from
        // tunneling through kinematic and static bodies. This setting is only
        // considered on dynamic bodies.
        // @warning You should use this flag sparingly since it increases processing time.
        //snowman.setBullet(true);
        createSnowmanJoint();

        ejector = new Ejector(factory, world, FACTOR);

        addRocks(nRockStep);
        addSnows(nSnowStep);

        intelLogo = new Sprite(atlas.findRegion("intel"));
        intelLogo.setPosition(SceneWidth - intelLogo.getRegionWidth(), GroundHeight);

        world.setContactListener(
                new ContactListener() {
                    @Override
                    public void beginContact(Contact contact) {
                    }

                    @Override
                    public void endContact(Contact contact) {
                    }

                    @Override
                    public void preSolve(Contact contact, Manifold oldManifold) {
                    }

                    @Override
                    public void postSolve(Contact contact, ContactImpulse impulse) {
                        IcyWorldSprite spriteA = (IcyWorldSprite) contact.getFixtureA().getBody().getUserData();
                        IcyWorldSprite spriteB = (IcyWorldSprite) contact.getFixtureB().getBody().getUserData();

                        if (snows.contains(spriteA) || snows.contains(spriteB))
                            return;

                        /*
                        for (ParticleRecord records : particleRecords) {
                            if (records.sprites.contains(spriteA) || records.sprites.contains(spriteB))
                                return;
                        }*/

                        IcyWorldSprite stone = ejector.getStone();
                        if (spriteA == stone || spriteB == stone) {
                            ejector.removeStone();
                            addParticles = true;
                            if (spriteB == snowman || spriteA == snowman) {
                                scored = true;
                            }
                        }
                    }
                }
        );
    }

    private void createWorldBox() {
        // create box of the world
        side = factory.create("ground", new Vector2(GroundWidth/2, GroundHeight/2), BodyType.StaticBody,
                0f, .01f, .8f, SpriteFactory.SpriteShapes.Chain);

        IcyWorldSprite leftWall = factory.create("snow", new Vector2(0, 0), BodyType.StaticBody,
                0f, .01f, .8f, SpriteFactory.SpriteShapes.Edge);
        leftWall.setVisible(false);
    }

    private void createFanJoint() {
        RevoluteJointDef jointDef = new RevoluteJointDef();
        Body fanBody = fan.getBody();
        jointDef.initialize(side.getBody(), fanBody, fanBody.getPosition());
        jointDef.enableMotor = true;
        jointDef.motorSpeed = FanSpeed * MathUtils.degreesToRadians;
        jointDef.maxMotorTorque = 6000;
        world.createJoint(jointDef);
    }

    private void createSnowmanJoint() {
        WeldJointDef jointDef = new WeldJointDef();
        jointDef.initialize(snowman.getBody(), rightMount.getBody(),
                snowman.getBody().getPosition().sub(new Vector2(0, SnowmanHeight / (2 * FACTOR))));
        jointDef.collideConnected = true;
        world.createJoint(jointDef);
    }

    private IcyWorldSprite createRock(String name, int x, int y) {
        return factory.create(name, new Vector2(x, y), BodyType.DynamicBody,
                .5f, .1f, .8f, SpriteFactory.SpriteShapes.Custom);
    }

    private void addRocks(int n) {
        nRocks += n;
        for (int i = 0; i < n; i++) {
            String name = "rock" + (i % 5 + 1);
            IcyWorldSprite rock = createRock(name,
                    RockWidth * (i % nRocksPerRow + 1) + EjectorLength,
                    SceneHeight - RockHeight * (i / nRocksPerRow + 1));
            rocks.add(rock);
        }
    }

    private void addSnows(int n) {
        nSnows += n;
        for (int i = 0; i < n; i++) {
            IcyWorldSprite snow = createSnow(i * SnowWidth + SnowmanWidth / 2,
                    (int) (SceneHeight - 10 - SceneHeight * Math.random() / 3));
            snows.add(snow);
        }
    }

    private IcyWorldSprite createSnow(int x, int y) {
        IcyWorldSprite snow = factory.create("snow", new Vector2(x, y),
                BodyType.DynamicBody, .5f, .7f, .001f,
                SpriteFactory.SpriteShapes.Circle);
        Body body = snow.getBody();
        body.setGravityScale(.05f);
        body.setLinearDamping(1);
        return snow;
    }

    private void applyWind() {
        float xForce = (float) Math.random();
        for (IcyWorldSprite snow : snows) {
            Body body = snow.getBody();
            if (body.getPosition().x * FACTOR < EjectorLength * 2) {
                body.applyLinearImpulse(new Vector2(xForce * body.getMass(), 0),
                        body.getPosition(), true);
            } else if (body.getPosition().x * FACTOR > SceneWidth - SnowmanWidth * 2) {
                body.applyLinearImpulse(new Vector2(-1 * xForce * body.getMass(), 0),
                        body.getPosition(), true);
            }
        }
    }

    private void createScoreAnimation() {
        Sprite sprite = new Sprite(atlas.findRegion("score"));
        sprite.setPosition(SceneWidth / 2 - sprite.getRegionWidth() / 2, 0);
        scoreSprites.add(sprite);
    }

    private class ParticleRecord {
        public Vector<IcyWorldSprite> sprites;
        public float life;

        public ParticleRecord(Vector<IcyWorldSprite> particles, float time) {
            sprites = particles;
            life = time;
        }
    }

    private LinkedList<ParticleRecord> particleRecords = new LinkedList<ParticleRecord>();
    private boolean addParticles = false;

    /*
     * unit is in pixels
     * number means #particles for each side
     */
    private Vector<IcyWorldSprite> createParticles(float x, float y, int width,
                                                  int number, Vector2 initVelocity) {
        Vector<IcyWorldSprite> sprites = new Vector<IcyWorldSprite>();
        //calculate the rectangle for particles
        float length = width * number;
        float ix = x - length / 2 + width / 2;
        float iy = y - length / 2 + width / 2;
        float maxForce = .1f;
        Vector2 center = new Vector2(x / FACTOR, y / FACTOR);
        for (int i = 0; i < number; i++) {
            for (int j = 0; j < number; j++) {
                IcyWorldSprite sprite = factory.create("particle",
                        new Vector2(ix, iy), BodyDef.BodyType.DynamicBody,
                        .5f, .7f, .7f, SpriteFactory.SpriteShapes.Circle);
                Body body = sprite.getBody();
                body.setLinearDamping(0.8f);
                Vector2 force = new Vector2((ix - x)/FACTOR, (iy - y)/FACTOR);
                body.setLinearVelocity(initVelocity);
                body.applyLinearImpulse(force.scl(maxForce), body.getPosition(), true);
                sprites.add(sprite);
                ix = ix + width;
            }
            ix = x - length / 2;
            iy = iy + width;
        }
        return sprites;
    }

}
