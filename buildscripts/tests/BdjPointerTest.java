package java.awt;

import java.awt.event.KeyEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.util.ArrayList;
import java.util.List;

/** Exercises the production helper with real AWT hit testing and event dispatch. */
public final class BdjPointerTest {
    private static final class Focus extends DefaultKeyboardFocusManager {
        private final Component target;

        Focus(Component target) {
            this.target = target;
        }

        protected Component getGlobalFocusOwner() {
            return target;
        }
    }

    private static final class Events extends MouseAdapter {
        final List<MouseEvent> received = new ArrayList<MouseEvent>();

        Events(Component target) {
            target.addMouseListener(this);
            target.addMouseMotionListener(this);
        }

        public void mouseMoved(MouseEvent event) { received.add(event); }
        public void mousePressed(MouseEvent event) { received.add(event); }
        public void mouseReleased(MouseEvent event) { received.add(event); }
        public void mouseClicked(MouseEvent event) { received.add(event); }
    }

    private static final class Scene {
        final Container root = new Container();
        final Component focused = new Component() {};
        final Container menu = new Container();
        final Component button = new Component() {};

        Scene() {
            root.setSize(640, 480);
            focused.setBounds(20, 40, 120, 70);
            menu.setBounds(220, 100, 180, 200);
            button.setBounds(15, 20, 90, 50);
            root.add(focused);
            root.add(menu);
            menu.add(button);
            // Create the real lightweight peers used by Container's event hit test.
            // Keep the root undisplayable so this headless fixture needs no window.
            focused.addNotify();
            menu.addNotify();
            KeyboardFocusManager.setCurrentKeyboardFocusManager(new Focus(focused));
        }

        void dispose() {
            focused.removeNotify();
            menu.removeNotify();
        }
    }

    private static void flushEvents() throws Exception {
        EventQueue.invokeAndWait(new Runnable() {
            public void run() {}
        });
    }

    private static void click(int x, int y) throws Exception {
        if (!BDJHelper.postMouseEvent(x, y)
            || !BDJHelper.postMouseEvent(MouseEvent.MOUSE_PRESSED)
            || !BDJHelper.postMouseEvent(MouseEvent.MOUSE_CLICKED)
            || !BDJHelper.postMouseEvent(MouseEvent.MOUSE_RELEASED)) {
            throw new AssertionError("Pointer event rejected");
        }
        flushEvents();
    }

    private static void assertClick(Events events, Component target, int x, int y) {
        int[] ids = {MouseEvent.MOUSE_MOVED, MouseEvent.MOUSE_PRESSED,
                     MouseEvent.MOUSE_CLICKED, MouseEvent.MOUSE_RELEASED};
        if (events.received.size() != ids.length) {
            throw new AssertionError("Expected one mouse sequence, received " + events.received.size());
        }
        for (int i = 0; i < ids.length; i++) {
            MouseEvent event = events.received.get(i);
            if (event.getSource() != target || event.getX() != x || event.getY() != y
                || event.getID() != ids[i]) {
                throw new AssertionError("Wrong source, component coordinates or sequence: " + event);
            }
        }
    }

    private static void pointerSelectsNestedButton() throws Exception {
        Scene scene = new Scene();
        try {
            Events focusedEvents = new Events(scene.focused);
            Events buttonEvents = new Events(scene.button);
            click(245, 130);
            if (!focusedEvents.received.isEmpty()) {
                throw new AssertionError("Pointer delivered to keyboard focus owner");
            }
            assertClick(buttonEvents, scene.button, 10, 10);
            if (!BDJHelper.postKeyEvent(KeyEvent.KEY_PRESSED, 0, KeyEvent.VK_DOWN)
                || BDToolkit.lastQueueTarget != scene.focused) {
                throw new AssertionError("Pointer changed keyboard event routing");
            }
            flushEvents();
        } finally {
            scene.dispose();
        }
    }

    private static void parentListenerReceivesPointer() throws Exception {
        Scene scene = new Scene();
        try {
            Events menuEvents = new Events(scene.menu);
            click(245, 130);
            assertClick(menuEvents, scene.menu, 25, 30);
        } finally {
            scene.dispose();
        }
    }

    private static void outsideGraphicsDoesNotClick() throws Exception {
        Scene scene = new Scene();
        try {
            Events menuEvents = new Events(scene.menu);
            if (BDJHelper.postMouseEvent(700, 500)
                || BDJHelper.postMouseEvent(MouseEvent.MOUSE_PRESSED)) {
                throw new AssertionError("Outside pointer accepted");
            }
            BDJHelper.postMouseEvent(MouseEvent.MOUSE_RELEASED);
            flushEvents();
            if (!menuEvents.received.isEmpty()) {
                throw new AssertionError("Outside pointer delivered to menu");
            }
        } finally {
            scene.dispose();
        }
    }

    private static void noFocusDoesNotDispatch() {
        KeyboardFocusManager.setCurrentKeyboardFocusManager(new Focus(null));
        if (BDJHelper.postMouseEvent(245, 130)) {
            throw new AssertionError("Unfocused pointer accepted");
        }
    }

    public static void main(String[] args) throws Exception {
        pointerSelectsNestedButton();
        parentListenerReceivesPointer();
        outsideGraphicsDoesNotClick();
        noFocusDoesNotDispatch();
        System.out.println("BD-J pointer target, local coordinates, parent listener and key routing passed");
    }
}

/** Only the BD-J context-to-event-queue boundary is substituted for this host fixture. */
final class BDToolkit {
    static Component lastQueueTarget;

    public static EventQueue getEventQueue(Component component) {
        lastQueueTarget = component;
        return Toolkit.getDefaultToolkit().getSystemEventQueue();
    }
}
