/*
  pygame - Python Game Library
  Copyright (C) 2000-2001  Pete Shinners

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  Pete Shinners
  pete@shinners.org
*/

/*
 *  pygame event module
 */
#define PYGAMEAPI_EVENT_INTERNAL
#include "pygame.h"
#include "pgcompat.h"
#include "doc/event_doc.h"

#include "structmember.h"

#include <limits.h>

/* SDL2 to Pygame event mapping
 *
 * Pygame events are based on SDL 1.2 events. Actual Pygame event type
 * integer values differ from the integer values of corresponding
 * SDL 2 event types.
 *
 *  Pygame                SDL2
 *
 *  PGE_NOEVENT           No corresponding event type
 *  PGE_ACTIVEEVENT       SDL_WINDOWEVENT_ENTER
 *                        SDL_WINDOWEVENT_LEAVE
 *                        SDL_WINDOWEVENT_FOCUS_GAIN
 *                        SDL_WINDOWEVENT_FOCUS_LOST
 *                        SDL_WINDOWEVENT_MINIMIZED
 *                        SDL_WINDOWEVENT_RETORED
 *  PGE_KEYDOWN           SDL_KEYDOWN
 *  PGE_KEYUP             SDL_KEYUP
 *  PGE_MOUSEMOTION       SDL_MOUSEMOTION
 *  PGE_MOUSEBUTTONDOWN   SDL_MOUSEBUTTONDOWN
 *  PGE_MOUSEBUTTONUP     SDL_MOUSEBUTTONUP
 *  PGE_JOYAXISMOTION     SDL_JOYAXISMOTION
 *  PGE_JOYBALLMOTION     SDL_JOYBALLMOTION
 *  PGE_JOYHATMOTION      SDL_JOYHATMOTION
 *  PGE_JOYBUTTONDOWN     SDL_JOYBUTTONDOWN
 *  PGE_JOYBUTTONUP       SDL_JOYBUTTONUP
 *  PGE_VIDEORESIZE       SDL_WINDOWEVENT_RESIZED
 *  PGE_VIDEOEXPOSE       SDL_WINDOWEVENT_EXPOSED
 *  PGE_QUIT              SDL_QUIT
 *  PGE_SYSWMEVENT        SDL_SYSWMEVENT
 *  pge_userevent         first_user_event
 *  PGE_NUMEVENTS         last_user_event + 1
 *  PGE_OTHEREVENT        any other event
 */

// FIXME: The system message code is only tested on windows, so only
//          include it there for now.
/* #include <SDL_syswm.h> */

/* Range allocated for user events.
 */
#define PGE_NON_SDL_EVENT ((SDL_EventType)-1)
static SDL_EventType first_user_event = PGE_NON_SDL_EVENT;
static SDL_EventType last_user_event = PGE_NON_SDL_EVENT;

/*this user event object is for safely passing
 *objects through the event queue.
 */

#define USEROBJECT_CHECK1 0xDEADBEEF
#define USEROBJECT_CHECK2 0xFEEDF00D

typedef struct UserEventObject
{
    struct UserEventObject* next;
    PyObject* object;
} UserEventObject;

static UserEventObject* user_event_objects = NULL;

#define PGE_ALLEVENTS ((Uint32)-1)
#define PGE_EVENTMASK(T) (1 << (T))

/* Key repeat handling; taken from SDL 1.2.15 source code in SDL_keyboard.c
 */
typedef struct {
    int delay;
    int interval;
    int firsttime;
    int timestamp;
} KeyRepeat;
static KeyRepeat repeat;

static int
Py_EnableKeyRepeat (int delay, int interval)
{
    if (delay < 0 || interval < 0)
    {
        SDL_SetError ("Negative key repeat value");
        return -1;
    }
    repeat.delay = delay;
    repeat.interval = interval;
    repeat.firsttime = 0;
    repeat.timestamp = 0;
    return 0;
}

static void
Py_GetKeyRepeat (int *delay, int *interval)
{
    *delay = repeat.delay;
    *interval = repeat.interval;
}

static int
Py_WaitEvent (SDL_Event *e)
{
    /* This function requires the GIL, but releases it during the wait.
     */
    KeyRepeat r = repeat;
    SDL_bool repeat_enabled = r.delay > 0 || r.interval > 0;
    int rcode = -1;
       
    Py_BEGIN_ALLOW_THREADS;
    while (SDL_WaitEvent (e) == 0)
    {
        if (e->type == SDL_KEYDOWN)
        {
            if (e->key.repeat)
            {
                if (!repeat_enabled)
                    continue;
                if (r.firsttime)
                {
                    if (r.timestamp - e->key.timestamp < r.delay)
                        continue;
                    r.firsttime = 0;
                    r.timestamp = e->key.timestamp;
                }
                else if (e->key.timestamp - r.timestamp < r.interval)
                    continue;
                else
                    r.timestamp = e->key.timestamp;
            }
            else if (repeat_enabled)
            {
                r.firsttime = 1;
                r.timestamp = e->key.timestamp;
            }
        }

        /* The loop only gets here if an event is to be returned.
         * Otherwise the loop was continued or an error occured.
         */
        rcode = 0;
        break;
    }
    Py_END_ALLOW_THREADS;
    repeat.firsttime = r.firsttime;
    repeat.timestamp = r.timestamp;

    return rcode;
}

static int
Py_PollEvent (SDL_Event* e)
{   
    SDL_bool repeat_enabled = repeat.delay > 0 || repeat.interval > 0;
    int rcode = 0;
       
    while (SDL_PollEvent (e) != 0)
    {
        if (e->type == SDL_KEYDOWN)
        {
            if (e->key.repeat)
            {
                if (!repeat_enabled)
                    continue;
                if (repeat.firsttime)
                {
                    if ((repeat.timestamp - e->key.timestamp) <
                        repeat.delay)
                        continue;
                    repeat.firsttime = 0;
                    repeat.timestamp = e->key.timestamp;
                }
                else if ((e->key.timestamp - repeat.timestamp) <
                         repeat.interval)
                    continue;
                else
                    repeat.timestamp = e->key.timestamp;
            }
            else if (repeat_enabled)
            {
                repeat.firsttime = 1;
                repeat.timestamp = e->key.timestamp;
            }
        }

        /* The loop only gets here if an event is to be returned.
         * Otherwise the loop was continued or an error occured.
         */
        rcode = 1;
        break;
    }

    return rcode;
}

/* SDL2 <=> Pygame event code translation
 */
static int
get_sdl_event_code (const PyEventObject* e)
{
    PyObject* dict = e->dict;
    PyObject* sdl_type = NULL;
    Py_ssize_t c = 0;
    int type = last_user_event + 1;

    if (!dict)
        goto finished;
    sdl_type = PyDict_GetItemString (dict, "sdl2_type");
    if (!sdl_type)
        goto finished;
    if (!PyNumber_Check (sdl_type))
        goto finished;
    c = PyNumber_AsSsize_t (sdl_type, NULL);
    if (c > last_user_event || c < INT_MIN)
        goto finished;
    type = (int)c;

finished:
    return type;
}

static int
sdl_to_pg (const SDL_Event* e)
{
    switch (e->type) {

    case SDL_WINDOWEVENT:
        switch (e->window.event) {

        case SDL_WINDOWEVENT_ENTER:
        case SDL_WINDOWEVENT_LEAVE:
        case SDL_WINDOWEVENT_FOCUS_GAINED:
        case SDL_WINDOWEVENT_FOCUS_LOST:
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            return PGE_ACTIVEEVENT;
        case SDL_WINDOWEVENT_RESIZED:
            return PGE_VIDEORESIZE;
        case SDL_WINDOWEVENT_EXPOSED:
            return PGE_VIDEOEXPOSE;
        default:
            break;
        }
        break;
    case SDL_KEYDOWN:
        return PGE_KEYDOWN;
    case SDL_KEYUP:
        return PGE_KEYUP;
    case SDL_MOUSEMOTION:
        return PGE_MOUSEMOTION;
    case SDL_MOUSEBUTTONDOWN:
        return PGE_MOUSEBUTTONDOWN;
    case SDL_MOUSEBUTTONUP:
        return PGE_MOUSEBUTTONUP;
    case SDL_JOYAXISMOTION:
        return PGE_JOYAXISMOTION;
    case SDL_JOYBALLMOTION:
        return PGE_JOYBALLMOTION;
    case SDL_JOYHATMOTION:
        return PGE_JOYHATMOTION;
    case SDL_JOYBUTTONDOWN:
        return PGE_JOYBUTTONDOWN;
    case SDL_JOYBUTTONUP:
        return PGE_JOYBUTTONUP;
    case SDL_QUIT:
        return PGE_QUIT;
    case SDL_SYSWMEVENT:
        return PGE_SYSWMEVENT;
    default: /* User events and others */
        if (e->type >= first_user_event &&
            e->type <= last_user_event)
            return e->type - first_user_event + PGE_USEREVENT;
    }

    return PGE_OTHEREVENT;
}

static SDL_EventType
pg_type_to_sdl (int pge_type)
{
    switch (pge_type) {

    case PGE_ACTIVEEVENT:
    case PGE_VIDEOEXPOSE:
    case PGE_VIDEORESIZE:
        return SDL_WINDOWEVENT;
    case PGE_KEYDOWN:
        return SDL_KEYDOWN;
    case PGE_KEYUP:
        return SDL_KEYUP;
    case PGE_MOUSEMOTION:
        return SDL_MOUSEMOTION;
    case PGE_MOUSEBUTTONDOWN:
        return SDL_MOUSEBUTTONDOWN;
    case PGE_MOUSEBUTTONUP:
        return SDL_MOUSEBUTTONUP;
    case PGE_JOYAXISMOTION:
        return SDL_JOYAXISMOTION;
    case PGE_JOYBALLMOTION:
        return SDL_JOYBALLMOTION;
    case PGE_JOYHATMOTION:
        return SDL_JOYHATMOTION;
    case PGE_JOYBUTTONDOWN:
        return SDL_JOYBUTTONDOWN;
    case PGE_JOYBUTTONUP:
        return SDL_JOYBUTTONUP;
    case PGE_QUIT:
        return SDL_QUIT;
    case PGE_SYSWMEVENT:
        return SDL_SYSWMEVENT;
    default:
        if (pge_type >= PGE_USEREVENT && pge_type < PGE_NUMEVENTS)
            return pge_type - PGE_USEREVENT + first_user_event;
    }
    return PGE_NON_SDL_EVENT;

}

static int
pg_to_sdl (const PyEventObject* e)
{
    int pge_type = e->type;

    if (pge_type == PGE_OTHEREVENT)
        return get_sdl_event_code (e);
    return pg_type_to_sdl (pge_type);
}

/*must pass dictionary as this object*/
static UserEventObject*
user_event_addobject (PyObject* obj)
{
    UserEventObject* userobj = PyMem_New (UserEventObject, 1);
    if (!userobj)
        return NULL;

    Py_INCREF (obj);
    userobj->next = user_event_objects;
    userobj->object = obj;
    user_event_objects = userobj;

    return userobj;
}

/*note, we doublecheck to make sure the pointer is in our list,
 *not just some random pointer. this will keep us safe(r).
 */
static PyObject*
user_event_getobject (UserEventObject* userobj)
{
    PyObject* obj = NULL;
    if (!user_event_objects) /*fail in most common case*/
        return NULL;
    if (user_event_objects == userobj)
    {
        obj = userobj->object;
        user_event_objects = userobj->next;
    }
    else
    {
        UserEventObject* hunt = user_event_objects;
        while (hunt && hunt->next != userobj)
            hunt = hunt->next;
        if (hunt)
        {
            hunt->next = userobj->next;
            obj = userobj->object;
        }
    }
    if (obj)
        PyMem_Del (userobj);
    return obj;
}

static void
user_event_cleanup (void)
{
    if (user_event_objects)
    {
        UserEventObject *hunt, *kill;
        hunt = user_event_objects;
        while (hunt)
        {
            kill = hunt;
            hunt = hunt->next;
            Py_DECREF (kill->object);
            PyMem_Del (kill);
        }
        user_event_objects = NULL;
    }
}

static int PyEvent_FillUserEvent (PyEventObject *e, SDL_Event *event)
{
    UserEventObject *userobj = user_event_addobject (e->dict);
    if (!userobj)
        return -1;

    event->type = e->type;
    event->user.code = USEROBJECT_CHECK1;
    event->user.data1 = (void*)USEROBJECT_CHECK2;
    event->user.data2 = userobj;
    return 0;
}

static PyTypeObject PyEvent_Type;
static PyObject* PyEvent_New (SDL_Event*);
static PyObject* PyEvent_New2 (int, PyObject*);
#define PyEvent_Check(x) ((x)->ob_type == &PyEvent_Type)

static char*
name_from_eventtype (int pge_type)
{
    switch (pge_type)
    {
    case PGE_ACTIVEEVENT:
        return "ActiveEvent";
    case PGE_KEYDOWN:
        return "KeyDown";
    case PGE_KEYUP:
        return "KeyUp";
    case PGE_MOUSEMOTION:
        return "MouseMotion";
    case PGE_MOUSEBUTTONDOWN:
        return "MouseButtonDown";
    case PGE_MOUSEBUTTONUP:
        return "MouseButtonUp";
    case PGE_JOYAXISMOTION:
        return "JoyAxisMotion";
    case PGE_JOYBALLMOTION:
        return "JoyBallMotion";
    case PGE_JOYHATMOTION:
        return "JoyHatMotion";
    case PGE_JOYBUTTONUP:
        return "JoyButtonUp";
    case PGE_JOYBUTTONDOWN:
        return "JoyButtonDown";
    case PGE_QUIT:
        return "Quit";
    case PGE_SYSWMEVENT:
        return "SysWMEvent";
    case PGE_VIDEORESIZE:
        return "VideoResize";
    case PGE_VIDEOEXPOSE:
        return "VideoExpose";
    case PGE_NOEVENT:
        return "NoEvent";
    }
    if (pge_type >= PGE_USEREVENT && pge_type < PGE_NUMEVENTS)
        return "UserEvent";
    return "Unknown";
}

/* Helper for adding objects to dictionaries. Check for errors with
   PyErr_Occurred() */
static void
insobj (PyObject *dict, char *name, PyObject *v)
{
    if(v)
    {
        PyDict_SetItemString (dict, name, v);
        Py_DECREF (v);
    }
}

#if defined(Py_USING_UNICODE)

static PyObject*
our_unichr (long uni)
{
    static PyObject* bltin_unichr = NULL;

    if (bltin_unichr == NULL)
    {
        PyObject* bltins;

        bltins = PyImport_ImportModule (BUILTINS_MODULE);
        bltin_unichr = PyObject_GetAttrString (bltins, BUILTINS_UNICHR);
        Py_DECREF (bltins);
    }
    return PyEval_CallFunction (bltin_unichr, "(l)", uni);
}

static PyObject*
our_empty_ustr (void)
{
    static PyObject* empty_ustr = NULL;

    if (empty_ustr == NULL)
    {
        PyObject* bltins;
        PyObject* bltin_unicode;

        bltins = PyImport_ImportModule (BUILTINS_MODULE);
        bltin_unicode = PyObject_GetAttrString (bltins, BUILTINS_UNICODE);
        empty_ustr = PyEval_CallFunction (bltin_unicode, "(s)", "");
        Py_DECREF (bltin_unicode);
        Py_DECREF (bltins);
    }

    Py_INCREF (empty_ustr);

    return empty_ustr;
}

#else

static PyObject*
our_unichr (long uni)
{
    return PyInt_FromLong (uni);
}

static PyObject*
our_empty_ustr (void)
{
    return PyInt_FromLong (0);
}

#endif /* Py_USING_UNICODE */

/* Convert a KEYDOWN event to a Python unicode string */
static PyObject*
key_to_unicode (const SDL_Keysym* key)
{
    static const SDL_Keymod ModMask = ~KMOD_SHIFT;
    SDL_Keycode c = key->sym;
    SDL_Keymod m = key->mod;

    if (c & 0x40000000)
        return our_empty_ustr ();
    if (m & ModMask)
        return our_empty_ustr ();
    if (m & KMOD_SHIFT)
        c = Py_UNICODE_TOUPPER (c);
    return our_unichr (c);
}

static PyObject*
dict_from_event (SDL_Event* event)
{
    PyObject *dict=NULL, *tuple, *obj;
    int hx, hy;
    int pge_type = sdl_to_pg (event);
    int gain = 0;
    int state = 0;

    /*check if it is an event the user posted*/
    if (event->user.code == USEROBJECT_CHECK1
        && event->user.data1 == (void*)USEROBJECT_CHECK2)
    {
        dict = user_event_getobject ((UserEventObject*)event->user.data2);
        if (dict)
            return dict;
    }

    if (!(dict = PyDict_New ()))
        return NULL;
    insobj (dict, "sdl2_type", PyInt_FromLong (event->type));
    if (event->type == SDL_WINDOWEVENT)
        insobj (dict,
                "window_id",
                PyInt_FromLong (event->window.windowID));
    switch (pge_type)
    {
    case PGE_ACTIVEEVENT:
        switch (event->window.event) {

        case SDL_WINDOWEVENT_ENTER:
            gain = 1;
            state = PGE_APPFOCUSMOUSE;
            break;
        case SDL_WINDOWEVENT_LEAVE:
            gain = 0;
            state = PGE_APPFOCUSMOUSE;
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            gain = 1;
            state = PGE_APPINPUTFOCUS;
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            gain = 0;
            state = PGE_APPINPUTFOCUS;
            break;
        case SDL_WINDOWEVENT_MINIMIZED:
            gain = 0;
            state = PGE_APPACTIVE;
            break;
        default:
            assert (event->window.event == SDL_WINDOWEVENT_RESTORED);
            gain = 1;
            state = PGE_APPACTIVE;
        }
        insobj (dict, "gain", PyInt_FromLong (gain));
        insobj (dict, "state", PyInt_FromLong (state));
        break;
    case PGE_KEYDOWN:
        insobj (dict, "unicode", key_to_unicode (&event->key.keysym));
        /* fall through */
    case PGE_KEYUP:
        insobj (dict, "key", PyInt_FromLong (event->key.keysym.sym));
        insobj (dict, "mod", PyInt_FromLong (event->key.keysym.mod));
        insobj (dict, "scancode", PyInt_FromLong (event->key.keysym.scancode));
        break;
    case PGE_MOUSEMOTION:
        obj = Py_BuildValue ("(ii)", event->motion.x, event->motion.y);
        insobj (dict, "pos", obj);
        obj = Py_BuildValue ("(ii)", event->motion.xrel, event->motion.yrel);
        insobj (dict, "rel", obj);
        if ((tuple = PyTuple_New (3)))
        {
            PyTuple_SET_ITEM
                (tuple, 0,
                 PyInt_FromLong ((event->motion.state&SDL_BUTTON(1)) != 0));
            PyTuple_SET_ITEM
                (tuple, 1,
                 PyInt_FromLong ((event->motion.state&SDL_BUTTON(2)) != 0));
            PyTuple_SET_ITEM
                (tuple, 2,
                 PyInt_FromLong ((event->motion.state&SDL_BUTTON(3)) != 0));
            insobj (dict, "buttons", tuple);
        }
        break;
    case PGE_MOUSEBUTTONDOWN:
    case PGE_MOUSEBUTTONUP:
        obj = Py_BuildValue ("(ii)", event->button.x, event->button.y);
        insobj (dict, "pos", obj);
        insobj (dict, "button", PyInt_FromLong (event->button.button));
        break;
    case PGE_JOYAXISMOTION:
        insobj (dict, "joy", PyInt_FromLong (event->jaxis.which));
        insobj (dict, "axis", PyInt_FromLong (event->jaxis.axis));
        insobj (dict, "value", PyFloat_FromDouble (event->jaxis.value/32767.0));
        break;
    case PGE_JOYBALLMOTION:
        insobj (dict, "joy", PyInt_FromLong (event->jball.which));
        insobj (dict, "ball", PyInt_FromLong (event->jball.ball));
        obj = Py_BuildValue ("(ii)", event->jball.xrel, event->jball.yrel);
        insobj (dict, "rel", obj);
        break;
    case PGE_JOYHATMOTION:
        insobj (dict, "joy", PyInt_FromLong (event->jhat.which));
        insobj (dict, "hat", PyInt_FromLong (event->jhat.hat));
        hx = hy = 0;
        if (event->jhat.value&SDL_HAT_UP)
            hy = 1;
        else if (event->jhat.value&SDL_HAT_DOWN)
            hy = -1;
        if (event->jhat.value&SDL_HAT_RIGHT)
            hx = 1;
        else if (event->jhat.value&SDL_HAT_LEFT)
            hx = -1;
        insobj (dict, "value", Py_BuildValue ("(ii)", hx, hy));
        break;
    case PGE_JOYBUTTONUP:
    case PGE_JOYBUTTONDOWN:
        insobj (dict, "joy", PyInt_FromLong (event->jbutton.which));
        insobj (dict, "button", PyInt_FromLong (event->jbutton.button));
        break;
    case PGE_VIDEORESIZE:
        obj = Py_BuildValue ("(ii)",
                             event->window.data1,
                             event->window.data2);
        insobj (dict, "size", obj);
        insobj (dict, "w", PyInt_FromLong (event->window.data1));
        insobj (dict, "h", PyInt_FromLong (event->window.data2));
        break;
    case PGE_SYSWMEVENT:
#ifdef WIN32
        insobj (dict, "hwnd", PyInt_FromLong ((long)(event-> syswm.msg->hwnd)));
        insobj (dict, "msg", PyInt_FromLong (event-> syswm.msg->msg));
        insobj (dict, "wparam", PyInt_FromLong (event-> syswm.msg->wParam));
        insobj (dict, "lparam", PyInt_FromLong (event-> syswm.msg->lParam));
#endif
        /*
         * Make the event
         */
#if (defined(unix) || defined(__unix__) || defined(_AIX)        \
     || defined(__OpenBSD__)) &&                                \
    (defined(PGE_VIDEO_DRIVER_X11) && !defined(__CYGWIN32__) &&         \
     !defined(ENABLE_NANOX) && !defined(__QNXNTO__))

        //printf("asdf :%d:", event->syswm.msg->event.xevent.type);
        insobj (dict,  "event",
               Bytes_FromStringAndSize
                ((char*) & (event->syswm.msg->event.xevent), sizeof (XEvent)));
#endif

        break;
        /* PGE_OTHEREVENT, PGE_VIDEOEXPOSE, and PGE_QUIT
         * have no event type specific attributes
         */
    }
    if (event->type == PGE_USEREVENT && event->user.code == 0x1000) {
        insobj (dict, "filename", Text_FromUTF8 (event->user.data1));
        free(event->user.data1);
        event->user.data1 = NULL;
    }
    if (event->type >= PGE_USEREVENT && event->type < PGE_NUMEVENTS)
        insobj (dict, "code", PyInt_FromLong (event->user.code));

    return dict;
}

/* event object internals */

static void
event_dealloc (PyObject* self)
{
    PyEventObject* e = (PyEventObject*)self;
    Py_XDECREF (e->dict);
    PyObject_DEL (self);
}

PyObject*
event_str (PyObject* self)
{
    PyEventObject* e = (PyEventObject*)self;
    char *str;
    PyObject *strobj;
    PyObject * pyobj;
    char *s;
    int size;
#if PY3
    PyObject *encodedobj;
#endif

    strobj = PyObject_Str (e->dict);
    if (strobj == NULL) {
        return NULL;
    }
#if PY3
    encodedobj = PyUnicode_AsUTF8String (strobj);
    Py_DECREF (strobj);
    strobj = encodedobj;
    encodedobj = NULL;
    if (strobj == NULL) {
        return NULL;
    }
    s = PyBytes_AsString (strobj);
#else
    s = PyString_AsString (strobj);
#endif
    size = (11 + strlen(name_from_eventtype (e->type)) + strlen(s) + sizeof(e->type) * 3 + 1);
    str = (char *) PyMem_Malloc(size);
    sprintf (str, "<Event(%d-%s %s)>", e->type, name_from_eventtype (e->type),
             s);

    Py_DECREF (strobj);

    pyobj = Text_FromUTF8 (str);
    PyMem_Free(str);

    return (pyobj);
}

static int
event_nonzero (PyEventObject *self)
{
    return self->type != PGE_NOEVENT;
}

static PyNumberMethods event_as_number = {
    (binaryfunc)NULL,                /*Add*/
    (binaryfunc)NULL,                /*subtract*/
    (binaryfunc)NULL,                /*multiply*/
#if !PY3
    (binaryfunc)NULL,                /*divide*/
#endif
    (binaryfunc)NULL,                /*remainder*/
    (binaryfunc)NULL,                /*divmod*/
    (ternaryfunc)NULL,               /*power*/
    (unaryfunc)NULL,                 /*negative*/
    (unaryfunc)NULL,                 /*pos*/
    (unaryfunc)NULL,                 /*abs*/
    (inquiry)event_nonzero,          /*nonzero*/
    (unaryfunc)NULL,                 /*invert*/
    (binaryfunc)NULL,                /*lshift*/
    (binaryfunc)NULL,                /*rshift*/
    (binaryfunc)NULL,                /*and*/
    (binaryfunc)NULL,                /*xor*/
    (binaryfunc)NULL,                /*or*/
#if !PY3
    (coercion)NULL,                  /*coerce*/
#endif
    (unaryfunc)NULL,                 /*int*/
#if !PY3
    (unaryfunc)NULL,                 /*long*/
#endif
    (unaryfunc)NULL,                 /*float*/
};

#define OFF(x) offsetof(PyEventObject, x)

static PyMemberDef event_members[] = {
    {"__dict__",  T_OBJECT, OFF(dict), READONLY},
    {"type",      T_INT,    OFF(type), READONLY},
    {"dict",      T_OBJECT, OFF(dict), READONLY},
    {NULL}  /* Sentinel */
};

/*
 * eventA == eventB
 * eventA != eventB
 */
static PyObject*
event_richcompare(PyObject *o1, PyObject *o2, int opid)
{
    PyEventObject *e1, *e2;

    if (!PyEvent_Check(o1) || !PyEvent_Check(o2))
    {
        goto Unimplemented;
    }

    e1 = (PyEventObject *) o1;
    e2 = (PyEventObject *) o2;
    switch (opid)
    {
    case Py_EQ:
        return PyBool_FromLong (e1->type == e2->type &&
                                PyObject_RichCompareBool (e1->dict,
                                                          e2->dict,
                                                          Py_EQ) == 1);
    case Py_NE:
        return PyBool_FromLong (e1->type != e2->type ||
                                PyObject_RichCompareBool (e1->dict,
                                                          e2->dict,
                                                          Py_NE) == 1);
    default:
        break;
    }

Unimplemented:
    Py_INCREF (Py_NotImplemented);
    return Py_NotImplemented;
}


static PyTypeObject PyEvent_Type =
{
    TYPE_HEAD (NULL, 0)
    "Event",                         /*name*/
    sizeof(PyEventObject),           /*basic size*/
    0,                               /*itemsize*/
    event_dealloc,                   /*dealloc*/
    0,                               /*print*/
    0,                               /*getattr*/
    0,                               /*setattr*/
    0,                               /*compare*/
    event_str,                       /*repr*/
    &event_as_number,                /*as_number*/
    0,                               /*as_sequence*/
    0,                               /*as_mapping*/
    (hashfunc)NULL,                  /*hash*/
    (ternaryfunc)NULL,               /*call*/
    (reprfunc)NULL,                  /*str*/
    PyObject_GenericGetAttr,         /* tp_getattro */
    PyObject_GenericSetAttr,         /* tp_setattro */
    0,                               /* tp_as_buffer */
#if PY3
    0,
#else
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_RICHCOMPARE,
#endif
    DOC_PYGAMEEVENTEVENT,            /* Documentation string */
    0,                               /* tp_traverse */
    0,                               /* tp_clear */
    event_richcompare,               /* tp_richcompare */
    0,                               /* tp_weaklistoffset */
    0,                               /* tp_iter */
    0,                               /* tp_iternext */
    0,                               /* tp_methods */
    event_members,                   /* tp_members */
    0,                               /* tp_getset */
    0,                               /* tp_base */
    0,                               /* tp_dict */
    0,                               /* tp_descr_get */
    0,                               /* tp_descr_set */
    offsetof(PyEventObject, dict),   /* tp_dictoffset */
    0,                               /* tp_init */
    0,                               /* tp_alloc */
    0,                               /* tp_new */
};

static PyObject*
PyEvent_New (SDL_Event* event)
{
    PyEventObject* e;
    e = PyObject_NEW (PyEventObject, &PyEvent_Type);
    if(!e)
        return NULL;

    if (event)
    {
        e->type = sdl_to_pg (event);
        e->dict = dict_from_event (event);
    }
    else
    {
        e->type = PGE_NOEVENT;
        e->dict = PyDict_New ();
    }
    return (PyObject*)e;
}

static PyObject*
PyEvent_New2 (int type, PyObject* dict)
{
    PyEventObject* e;
    e = PyObject_NEW (PyEventObject, &PyEvent_Type);
    if (e)
    {
        e->type = type;
        if (!dict)
            dict = PyDict_New ();
        else
            Py_INCREF (dict);
        e->dict = dict;
    }
    return (PyObject*)e;
}

/* event module functions */
static PyObject*
Event (PyObject* self, PyObject* arg, PyObject* keywords)
{
    PyObject* dict = NULL;
    PyObject* event;
    int type;
    if (!PyArg_ParseTuple (arg, "i|O!", &type, &PyDict_Type, &dict))
        return NULL;

    if (!dict)
        dict = PyDict_New ();
    else
        Py_INCREF (dict);

    if (keywords)
    {
        PyObject *key, *value;
        Py_ssize_t pos  = 0;
        while (PyDict_Next (keywords, &pos, &key, &value))
            PyDict_SetItem (dict, key, value);
    }

    event = PyEvent_New2 (type, dict);

    Py_DECREF (dict);
    return event;
}

static PyObject*
event_name (PyObject* self, PyObject* arg)
{
    int type;

    if (!PyArg_ParseTuple (arg, "i", &type))
        return NULL;

    return Text_FromUTF8 (name_from_eventtype (type));
}

static PyObject*
set_grab (PyObject* self, PyObject* arg)
{
    int doit;
    SDL_Window* win = NULL;
    if (!PyArg_ParseTuple (arg, "i", &doit))
        return NULL;
    VIDEO_INIT_CHECK ();

    win = Py_GetDefaultWindow ();
    if (win)
    {
        if (doit)
            SDL_SetWindowGrab (win, SDL_TRUE);
        else
            SDL_SetWindowGrab (win, SDL_FALSE);
    }

    Py_RETURN_NONE;
}

static PyObject*
get_grab (PyObject* self, PyObject* arg)
{
    SDL_Window* win;
    SDL_bool mode = SDL_FALSE;

    VIDEO_INIT_CHECK ();
    win = Py_GetDefaultWindow ();
    if (win)
        mode = SDL_GetWindowGrab (win);
    return PyInt_FromLong (mode);
}

static PyObject*
pygame_pump (PyObject* self, PyObject* args)
{
    VIDEO_INIT_CHECK ();
    SDL_PumpEvents ();
    Py_RETURN_NONE;
}

static PyObject*
pygame_wait (PyObject* self, PyObject* args)
{
    SDL_Event event;
    int status;

    VIDEO_INIT_CHECK ();

    status = Py_WaitEvent (&event);

    if (!status)
        return RAISE (PyExc_SDLError, SDL_GetError ());

    return PyEvent_New (&event);
}

static PyObject*
pygame_poll (PyObject* self, PyObject* args)
{
    SDL_Event event;

    VIDEO_INIT_CHECK ();

    if (Py_PollEvent (&event))
        return PyEvent_New (&event);
    return PyEvent_New (NULL);
}

static int
PG_PeepEvents (SDL_Event* events,
               int numevents,
               SDL_eventaction action,
               Uint32 mask)
{
    int n;
    int tally;
    int type;
    Uint32 sdl_type;

    for (type = 1, tally = 0, n = 0;
         tally < numevents && type != PGE_NUMEVENTS;
         ++type, tally += n, n = 0) {
        if (((Uint32)1 << type) & mask) {
            sdl_type = pg_type_to_sdl (type);
            if (sdl_type != PGE_NON_SDL_EVENT)
                n = SDL_PeepEvents (events,
                                    numevents,
                                    action,
                                    sdl_type, sdl_type);
            if (n == -1)
                return -1;
        }
    }
    return tally;
}

static PyObject*
event_clear (PyObject* self, PyObject* args)
{
    SDL_Event event;
    Uint32 mask = 0;
    int loop, num;
    PyObject* type;
    int val;

    if (PyTuple_Size (args) != 0 && PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "get requires 0 or 1 argument");

    VIDEO_INIT_CHECK ();

    if (PyTuple_Size (args) == 0)
        mask = PGE_ALLEVENTS;
    else
    {
        type = PyTuple_GET_ITEM (args, 0);
        if(PySequence_Check (type))
        {
            num = PySequence_Size (type);
            for(loop = 0; loop < num; ++loop)
            {
                if (!IntFromObjIndex (type, loop, &val))
                    return RAISE
                        (PyExc_TypeError,
                         "type sequence must contain valid event types");
                mask |= PGE_EVENTMASK (val);
            }
        }
        else if (IntFromObj (type, &val))
            mask = PGE_EVENTMASK (val);
        else
            return RAISE (PyExc_TypeError,
                          "get type must be numeric or a sequence");
    }

    SDL_PumpEvents ();

    while (PG_PeepEvents (&event, 1, SDL_GETEVENT, mask) == 1)
    {}

    Py_RETURN_NONE;
}

static PyObject*
event_get (PyObject* self, PyObject* args)
{
    SDL_Event event;
    Uint32 mask = 0;
    int loop, num;
    PyObject* type, *list, *e;
    int val;

    if (PyTuple_Size (args) != 0 && PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "get requires 0 or 1 argument");

    VIDEO_INIT_CHECK ();

    if (PyTuple_Size (args) == 0)
        mask = PGE_ALLEVENTS;
    else
    {
        type = PyTuple_GET_ITEM (args, 0);
        if (PySequence_Check (type))
        {
            num = PySequence_Size (type);
            for(loop = 0; loop < num; ++loop)
            {
                if (!IntFromObjIndex (type, loop, &val))
                    return RAISE
                        (PyExc_TypeError,
                         "type sequence must contain valid event types");
                mask |= PGE_EVENTMASK (val);
            }
        }
        else if (IntFromObj (type, &val))
            mask = PGE_EVENTMASK (val);
        else
            return RAISE (PyExc_TypeError,
                          "get type must be numeric or a sequence");
    }

    list = PyList_New (0);
    if (!list)
        return NULL;

    SDL_PumpEvents ();

    while (PG_PeepEvents (&event, 1, SDL_GETEVENT, mask) == 1)
    {
        e = PyEvent_New (&event);
        if (!e)
        {
            Py_DECREF (list);
            return NULL;
        }

        PyList_Append (list, e);
        Py_DECREF (e);
    }
    return list;
}

static PyObject*
event_peek (PyObject* self, PyObject* args)
{
    SDL_Event event;
    int result;
    Uint32 mask = 0;
    int loop, num, noargs=0;
    PyObject* type;
    int val;

    if (PyTuple_Size (args) != 0 && PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "peek requires 0 or 1 argument");

    VIDEO_INIT_CHECK ();

    if (PyTuple_Size (args) == 0)
    {
        mask = PGE_ALLEVENTS;
        noargs=1;
    }
    else
    {
        type = PyTuple_GET_ITEM (args, 0);
        if(PySequence_Check (type))
        {
            num = PySequence_Size (type);
            for(loop = 0; loop < num; ++loop)
            {
                if (!IntFromObjIndex (type, loop, &val))
                    return RAISE
                        (PyExc_TypeError,
                         "type sequence must contain valid event types");
                mask |= PGE_EVENTMASK (val);
            }
        }
        else if (IntFromObj (type, &val))
            mask = PGE_EVENTMASK (val);
        else
            return RAISE (PyExc_TypeError,
                          "peek type must be numeric or a sequence");
    }

    SDL_PumpEvents ();
    result = PG_PeepEvents (&event, 1, SDL_PEEKEVENT, mask);

    if (noargs)
        return PyEvent_New (&event);
    return PyInt_FromLong (result == 1);
}

static PyObject*
event_post (PyObject* self, PyObject* args)
{
    PyEventObject* e;
    SDL_Event event;
    int isblocked = 0;
    int sdl_type;

    if (!PyArg_ParseTuple (args, "O!", &PyEvent_Type, &e))
        return NULL;

    VIDEO_INIT_CHECK ();

    sdl_type = pg_to_sdl (e);

    /* see if the event is blocked before posting it. */
    isblocked = SDL_EventState (sdl_type, SDL_QUERY) == SDL_IGNORE;

    if (isblocked) {
        /* event is blocked, so we do not post it. */
        Py_RETURN_NONE;
    }

    if (PyEvent_FillUserEvent (e, &event))
        return NULL;

    if (!SDL_PushEvent (&event))
        return RAISE (PyExc_SDLError, "Event queue full");

    Py_RETURN_NONE;
}

static int
CheckEventInRange(int evt)
{
    return evt >= 0 && evt < PGE_NUMEVENTS;
}

static PyObject*
set_allowed (PyObject* self, PyObject* args)
{
    int loop, num;
    PyObject* type;
    int val;

    if (PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "set_allowed requires 1 argument");

    VIDEO_INIT_CHECK ();

    type = PyTuple_GET_ITEM (args, 0);
    if (PySequence_Check (type))
    {
        num = PySequence_Length (type);
        for(loop = 0; loop < num; ++loop)
        {
            if (!IntFromObjIndex (type, loop, &val))
                return RAISE (PyExc_TypeError,
                              "type sequence must contain valid event types");
            if(!CheckEventInRange(val))
                return RAISE (PyExc_ValueError, "Invalid event in sequence");
            SDL_EventState ((Uint8)val, SDL_ENABLE);
        }
    }
    else if (type == Py_None)
        SDL_EventState ((Uint8)0xFF, SDL_IGNORE);
    else if (IntFromObj (type, &val))
    {
        if(!CheckEventInRange (val))
            return RAISE (PyExc_ValueError, "Invalid event");
        SDL_EventState ((Uint8)val, SDL_ENABLE);
    }
    else
        return RAISE (PyExc_TypeError, "type must be numeric or a sequence");

    Py_RETURN_NONE;
}

static PyObject*
set_blocked (PyObject* self, PyObject* args)
{
    int loop, num;
    PyObject* type;
    int val;

    if (PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "set_blocked requires 1 argument");

    VIDEO_INIT_CHECK ();

    type = PyTuple_GET_ITEM (args, 0);
    if (PySequence_Check (type))
    {
        num = PySequence_Length (type);
        for(loop = 0; loop < num; ++loop)
        {
            if (!IntFromObjIndex (type, loop, &val))
                return RAISE (PyExc_TypeError,
                              "type sequence must contain valid event types");
            if(!CheckEventInRange(val))
                return RAISE (PyExc_ValueError, "Invalid event in sequence");
            SDL_EventState ((Uint8)val, SDL_IGNORE);
        }
    }
    else if (type == Py_None)
        SDL_EventState ((Uint8)0xFF, SDL_IGNORE);
    else if (IntFromObj (type, &val))
    {
        if(!CheckEventInRange (val))
            return RAISE (PyExc_ValueError, "Invalid event");
        SDL_EventState ((Uint8)val, SDL_IGNORE);
    }
    else
        return RAISE (PyExc_TypeError, "type must be numeric or a sequence");

    Py_RETURN_NONE;
}

static PyObject*
get_blocked (PyObject* self, PyObject* args)
{
    int loop, num;
    PyObject* type;
    int val;
    int isblocked = 0;

    if (PyTuple_Size (args) != 1)
        return RAISE (PyExc_ValueError, "get_blocked requires 1 argument");

    VIDEO_INIT_CHECK ();

    type = PyTuple_GET_ITEM (args, 0);
    if (PySequence_Check (type))
    {
        num = PySequence_Length (type);
        for (loop = 0; loop < num; ++loop)
        {
            if (!IntFromObjIndex (type, loop, &val))
                return RAISE (PyExc_TypeError,
                              "type sequence must contain valid event types");
            if(!CheckEventInRange(val))
                return RAISE (PyExc_ValueError, "Invalid event in sequence");
            isblocked |= SDL_EventState ((Uint8)val, SDL_QUERY) == SDL_IGNORE;
        }
    }
    else if (IntFromObj (type, &val))
    {
        if(!CheckEventInRange (val))
            return RAISE (PyExc_ValueError, "Invalid event");
        isblocked = SDL_EventState ((Uint8)val, SDL_QUERY) == SDL_IGNORE;
    }
    else
        return RAISE (PyExc_TypeError, "type must be numeric or a sequence");

    return PyInt_FromLong (isblocked);
}

static PyMethodDef _event_methods[] =
{
    { "Event", (PyCFunction)Event, 3, DOC_PYGAMEEVENTEVENT },
    { "event_name", event_name, METH_VARARGS, DOC_PYGAMEEVENTEVENTNAME },

    { "set_grab", set_grab, METH_VARARGS, DOC_PYGAMEEVENTSETGRAB },
    { "get_grab", (PyCFunction) get_grab, METH_NOARGS, DOC_PYGAMEEVENTGETGRAB },

    { "pump", (PyCFunction) pygame_pump, METH_NOARGS, DOC_PYGAMEEVENTPUMP },
    { "wait", (PyCFunction) pygame_wait, METH_NOARGS, DOC_PYGAMEEVENTWAIT },
    { "poll", (PyCFunction) pygame_poll, METH_NOARGS, DOC_PYGAMEEVENTPOLL },
    { "clear", event_clear, METH_VARARGS, DOC_PYGAMEEVENTCLEAR },
    { "get", event_get, METH_VARARGS, DOC_PYGAMEEVENTGET },
    { "peek", event_peek, METH_VARARGS, DOC_PYGAMEEVENTPEEK },
    { "post", event_post, METH_VARARGS, DOC_PYGAMEEVENTPOST },

    { "set_allowed", set_allowed, METH_VARARGS, DOC_PYGAMEEVENTSETALLOWED },
    { "set_blocked", set_blocked, METH_VARARGS, DOC_PYGAMEEVENTSETBLOCKED },
    { "get_blocked", get_blocked, METH_VARARGS, DOC_PYGAMEEVENTGETBLOCKED },

    { NULL, NULL, 0, NULL }
};

#if PY3
static struct PyModuleDef _module = {
    PyModuleDef_HEAD_INIT,
    "event",
    DOC_PYGAMEEVENT,
    0,
    _event_methods,
    NULL, NULL, NULL, NULL
};
#endif

MODINIT_DEFINE (event)
{
    PyObject *module, *dict, *apiobj;
    int ecode;
    static void* c_api[PYGAMEAPI_EVENT_NUMSLOTS];

    /* imported needed apis; Do this first so if there is an error
       the module is not loaded.
    */
    import_pygame_base ();
    if (PyErr_Occurred ()) {
        MODINIT_ERROR;
    }

    import_pygame_display ();
    if (PyErr_Occurred ()) {
        MODINIT_ERROR;
    }

    /* type preparation */
    if (PyType_Ready (&PyEvent_Type) < 0) {
        MODINIT_ERROR;
    }

    /* create the module */
#if PY3
    module = PyModule_Create (&_module);
#else
    module = Py_InitModule3 (MODPREFIX "event", _event_methods, DOC_PYGAMEEVENT);
#endif
    dict = PyModule_GetDict (module);

    if (PyDict_SetItemString (dict, "EventType",
                              (PyObject *)&PyEvent_Type) == -1) {
        DECREF_MOD (module);
        MODINIT_ERROR;
    }

    if (first_user_event != PGE_NON_SDL_EVENT) {
        int numevents = PGE_NUMEVENTS - PGE_USEREVENT + 1;
        first_user_event = SDL_RegisterEvents (numevents);
        if (first_user_event != PGE_NON_SDL_EVENT) {
            last_user_event = first_user_event + numevents - 1;
        }
    }
    /* export the c api */
    c_api[0] = &PyEvent_Type;
    c_api[1] = PyEvent_New;
    c_api[2] = PyEvent_New2;
    c_api[3] = PyEvent_FillUserEvent;
    c_api[4] = Py_EnableKeyRepeat;
    c_api[5] = Py_GetKeyRepeat;
    apiobj = encapsulate_api (c_api, "event");
    if (apiobj == NULL) {
        DECREF_MOD (module);
        MODINIT_ERROR;
    }
    ecode = PyDict_SetItemString (dict, PYGAMEAPI_LOCAL_ENTRY, apiobj);
    Py_DECREF (apiobj);
    if (ecode) {
        DECREF_MOD (module);
        MODINIT_ERROR;
    }

    /* Assume if there are events in the user events list
     * there is also a registered cleanup callback for them.
     */
    if (user_event_objects == NULL) {
        PyGame_RegisterQuit (user_event_cleanup);
    }
    MODINIT_RETURN (module);
}
