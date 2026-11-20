#!/usr/bin/env python3
"""Build the 3D assets of the web viewer with Blender: seamless surface textures and rigged character models.

Run inside Blender (no GUI needed):
    blender -b -P sim/tools/gen_assets.py -- --data sim/data --out sim/web/assets
or with the `bpy` module from pip:
    python sim/tools/gen_assets.py --data sim/data --out sim/web/assets

Textures: every module has a theme (floor and wall). Each surface is built from shader nodes and baked to a
diffuse map and a tangent-space normal map, then made seamless so it tiles. `--new-textures` adds another
variant of every theme (a different noise seed and a small colour shift) and keeps the old ones, so the viewer
can switch between them.

Characters: one low-poly, rigged model per hero (global cast and every module cast), written once and kept:
the same id always gives the same model (build/colours/props come from the id and the class), and an existing
.glb is never overwritten unless `--rebuild-characters` is given. Each has an Idle and a Walk animation and
its own baked cloth texture.

Output (all paths relative to --out):
    manifest.json                    what exists; the viewer reads this
    textures/<theme>_<surface>_v<N>_<d|n>.png
    textures/char_<id>.png
    models/<id>.glb
"""
import argparse
import colorsys
import hashlib
import json
import math
import os
import random
import sys

import bpy
import numpy as np

# ---------------------------------------------------------------- themes
THEMES = {
    "classic": dict(label="Classic delve", floor=("flagstone", ["#4a4650", "#6b6672", "#2a2830"]), wall=("brick", ["#5a4a44", "#7a645a", "#2b2420"]), scifi=False, accent="#c9a24a"),
    "ravenmoor": dict(label="The Dread of Ravenmoor", floor=("cobble", ["#3d4540", "#5c665c", "#1d2220"]), wall=("brick", ["#3a3a46", "#555568", "#1a1a22"]), scifi=False, accent="#8a2c3a"),
    "cyberhood": dict(label="CyberHood", floor=("plate", ["#1c2030", "#2e3550", "#ff2bd6"]), wall=("plate", ["#242040", "#3b3466", "#2bd9ff"]), scifi=True, accent="#ff2bd6"),
    "hollow_crown": dict(label="The Hollow Crown", floor=("flagstone", ["#3b3a2e", "#5f5c46", "#1b1a12"]), wall=("brick", ["#4a3f2c", "#6a5a3c", "#1f1a10"]), scifi=False, accent="#d9b13b"),
    "sunless_abbey": dict(label="The Sunless Abbey", floor=("moss", ["#2c3a2c", "#46603f", "#141c14"]), wall=("cobble", ["#3a3d38", "#5b6157", "#161815"]), scifi=False, accent="#6fbf73"),
    "meridian": dict(label="The Meridian", floor=("plate", ["#8a96a6", "#b5c0cf", "#37d6ff"]), wall=("plate", ["#6b7788", "#9aa7b8", "#2a303a"]), scifi=True, accent="#37d6ff"),
}

CLASS_STYLE = {
    "fighter": dict(cloth="#3a4f7a", trim="#b9bcc6", prop="sword"),
    "wizard": dict(cloth="#4b2f86", trim="#e8c14d", prop="staff"),
    "cleric": dict(cloth="#d8d4c8", trim="#d4a82e", prop="mace"),
    "rogue": dict(cloth="#2b2f35", trim="#5a7a52", prop="dagger"),
    "ranger": dict(cloth="#3f5f3a", trim="#7a5a36", prop="bow"),
    "barbarian": dict(cloth="#7a4a2a", trim="#a33a2a", prop="axe"),
}
SKIN = ["#f1c9a5", "#e0ac84", "#c68a5e", "#a8693f", "#7b4a2b", "#5a3520"]
HAIR = ["#1b1410", "#3a2616", "#6b4423", "#a8742f", "#d8c08a", "#8a8a90", "#7a2a1a"]


# ---------------------------------------------------------------- small helpers
def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


def lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexc(h, a=1.0):
    r, g, b = hexrgb(h)
    return (lin(r), lin(g), lin(b), a)


def shift(h, variant):
    """Variant 1 is the base palette; later variants rotate the hue a little and vary the brightness."""
    if variant <= 1:
        return h
    r, g, b = hexrgb(h)
    hh, s, v = colorsys.rgb_to_hsv(r, g, b)
    rnd = random.Random(variant * 7919 + int(hh * 360))
    hh = (hh + rnd.uniform(-0.06, 0.06)) % 1.0
    v = min(1.0, max(0.05, v * rnd.uniform(0.85, 1.15)))
    r, g, b = colorsys.hsv_to_rgb(hh, s, v)
    return "#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255))


def seed_of(text):
    return int(hashlib.md5(text.encode()).hexdigest()[:8], 16)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = 1
    sc.cycles.use_denoising = False
    return sc


# ---------------------------------------------------------------- surface materials (shader nodes)
class Surface:
    def __init__(self, name, seed, scale):
        self.mat = bpy.data.materials.new(name)
        self.mat.use_nodes = True
        self.nt = self.mat.node_tree
        self.nt.nodes.clear()
        self.seed = (seed % 997) * 0.713
        tc = self.node("ShaderNodeTexCoord")
        self.mp = self.node("ShaderNodeMapping")
        self.mp.inputs["Scale"].default_value = (scale, scale, scale)
        self.mp.inputs["Location"].default_value = (0, 0, self.seed)
        self.link(tc.outputs["UV"], self.mp.inputs["Vector"])

    def node(self, kind, **props):
        n = self.nt.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def link(self, a, b):
        self.nt.links.new(a, b)

    def noise(self, scale, detail=6.0, rough=0.55):
        n = self.node("ShaderNodeTexNoise", noise_dimensions="3D")
        n.inputs["Scale"].default_value = scale
        n.inputs["Detail"].default_value = detail
        n.inputs["Roughness"].default_value = rough
        self.link(self.mp.outputs[0], n.inputs["Vector"])
        return n

    def voronoi(self, scale, feature):
        v = self.node("ShaderNodeTexVoronoi", voronoi_dimensions="3D", feature=feature)
        v.inputs["Scale"].default_value = scale
        self.link(self.mp.outputs[0], v.inputs["Vector"])
        return v

    def mix(self, a, b, fac, blend="MIX"):
        m = self.node("ShaderNodeMix", data_type="RGBA", blend_type=blend)
        for sock, val in ((m.inputs[6], a), (m.inputs[7], b)):
            if isinstance(val, tuple):
                sock.default_value = val
            else:
                self.link(val, sock)
        if isinstance(fac, (int, float)):
            m.inputs[0].default_value = fac
        else:
            self.link(fac, m.inputs[0])
        return m.outputs[2]

    def ramp(self, src, lo, hi):
        r = self.node("ShaderNodeValToRGB")
        r.color_ramp.elements[0].position = lo
        r.color_ramp.elements[1].position = hi
        r.color_ramp.elements[0].color = (0, 0, 0, 1)
        r.color_ramp.elements[1].color = (1, 1, 1, 1)
        self.link(src, r.inputs["Fac"])
        return r.outputs["Color"]

    def bw(self, src):
        b = self.node("ShaderNodeRGBToBW")
        self.link(src, b.inputs["Color"])
        return b.outputs["Val"]

    def finish(self, color, height, strength=1.0, distance=0.08):
        out = self.node("ShaderNodeOutputMaterial")
        bsdf = self.node("ShaderNodeBsdfPrincipled")
        self.link(color, bsdf.inputs["Base Color"])
        bump = self.node("ShaderNodeBump")
        bump.inputs["Strength"].default_value = strength
        bump.inputs["Distance"].default_value = distance
        self.link(height, bump.inputs["Height"])
        self.link(bump.outputs["Normal"], bsdf.inputs["Normal"])
        self.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
        return self.mat


def darken(s, col, noise, amount):
    """Darken a colour by a grey noise so the surface is grainy without shifting its hue."""
    m = s.node("ShaderNodeMath", operation="MULTIPLY")
    s.link(noise.outputs["Fac"], m.inputs[0])
    m.inputs[1].default_value = amount
    return s.mix(col, (0, 0, 0, 1), m.outputs[0])


def stones(s, pal, cells, mortar_width):
    """Irregular stones with dark mortar between them: the colour and the height."""
    edge = s.voronoi(cells, "DISTANCE_TO_EDGE")
    cell = s.voronoi(cells, "F1")
    tone = s.bw(cell.outputs["Color"])
    base = s.mix(hexc(pal[0]), hexc(pal[1]), tone)
    grain = s.noise(24, 8, 0.6)
    base = darken(s, base, grain, 0.6)
    gap = s.ramp(edge.outputs["Distance"], 0.0, mortar_width)
    col = s.mix(hexc(pal[2]), base, gap)
    hmath = s.node("ShaderNodeMath", operation="MULTIPLY_ADD")
    s.link(grain.outputs["Fac"], hmath.inputs[0])
    hmath.inputs[1].default_value = 0.25
    s.link(gap, hmath.inputs[2])
    return col, hmath.outputs[0]


def build_surface(kind, pal, seed, name):
    if kind == "cobble":
        s = Surface(name, seed, 1.0)
        col, h = stones(s, pal, 7.0, 0.09)
        return s.finish(col, h, 1.2, 0.06)
    if kind == "flagstone":
        s = Surface(name, seed, 1.0)
        col, h = stones(s, pal, 3.0, 0.05)
        return s.finish(col, h, 0.9, 0.05)
    if kind == "brick":
        s = Surface(name, seed, 1.0)
        b = s.node("ShaderNodeTexBrick", offset=0.5, offset_frequency=2, squash=1.0, squash_frequency=2)
        b.inputs["Color1"].default_value = hexc(pal[0])
        b.inputs["Color2"].default_value = hexc(pal[1])
        b.inputs["Mortar"].default_value = hexc(pal[2])
        b.inputs["Scale"].default_value = 4.0
        b.inputs["Mortar Size"].default_value = 0.025
        b.inputs["Mortar Smooth"].default_value = 0.1
        b.inputs["Brick Width"].default_value = 0.5
        b.inputs["Row Height"].default_value = 0.25
        s.link(s.mp.outputs[0], b.inputs["Vector"])
        grain = s.noise(30, 8, 0.6)
        col = darken(s, b.outputs["Color"], grain, 0.5)
        h = s.node("ShaderNodeMath", operation="MULTIPLY_ADD")
        s.link(grain.outputs["Fac"], h.inputs[0])
        h.inputs[1].default_value = 0.08
        s.link(b.outputs["Fac"], h.inputs[2])
        return s.finish(col, h.outputs[0], 1.6, 0.1)
    if kind == "plate":
        s = Surface(name, seed, 1.0)
        b = s.node("ShaderNodeTexBrick", offset=0.0, offset_frequency=2, squash=1.0, squash_frequency=2)
        b.inputs["Color1"].default_value = hexc(pal[0])
        b.inputs["Color2"].default_value = hexc(pal[1])
        b.inputs["Mortar"].default_value = hexc(pal[2])
        b.inputs["Scale"].default_value = 3.0
        b.inputs["Mortar Size"].default_value = 0.022
        b.inputs["Mortar Smooth"].default_value = 0.0
        b.inputs["Brick Width"].default_value = 1.0
        b.inputs["Row Height"].default_value = 1.0
        s.link(s.mp.outputs[0], b.inputs["Vector"])
        scratch = s.noise(60, 10, 0.7)
        col = darken(s, b.outputs["Color"], scratch, 0.35)
        h = s.node("ShaderNodeMath", operation="MULTIPLY_ADD")
        s.link(scratch.outputs["Fac"], h.inputs[0])
        h.inputs[1].default_value = 0.03
        s.link(b.outputs["Fac"], h.inputs[2])
        return s.finish(col, h.outputs[0], 2.0, 0.12)
    # moss over stone
    s = Surface(name, seed, 1.0)
    big = s.noise(5, 8, 0.55)
    col = s.mix(hexc(pal[0]), hexc(pal[1]), big.outputs["Fac"])
    pools = s.noise(11, 6, 0.5)
    col = s.mix(col, hexc(pal[2]), s.ramp(pools.outputs["Fac"], 0.55, 0.7))
    fine = s.noise(40, 8, 0.7)
    col = darken(s, col, fine, 0.4)
    h = s.node("ShaderNodeMath", operation="MULTIPLY_ADD")
    s.link(fine.outputs["Fac"], h.inputs[0])
    h.inputs[1].default_value = 0.5
    s.link(big.outputs["Fac"], h.inputs[2])
    return s.finish(col, h.outputs[0], 1.0, 0.1)


def weave(hexcol, seed, name):
    s = Surface(name, seed, 1.0)
    w1 = s.node("ShaderNodeTexWave", wave_type="BANDS", bands_direction="X")
    w1.inputs["Scale"].default_value = 28
    w1.inputs["Distortion"].default_value = 1.5
    s.link(s.mp.outputs[0], w1.inputs["Vector"])
    w2 = s.node("ShaderNodeTexWave", wave_type="BANDS", bands_direction="Y")
    w2.inputs["Scale"].default_value = 28
    w2.inputs["Distortion"].default_value = 1.5
    s.link(s.mp.outputs[0], w2.inputs["Vector"])
    both = s.node("ShaderNodeMath", operation="ADD")
    s.link(w1.outputs["Fac"], both.inputs[0])
    s.link(w2.outputs["Fac"], both.inputs[1])
    n = s.noise(12, 5, 0.5)
    dark = shift_value(hexcol, 0.55)
    col = s.mix(hexc(dark), hexc(hexcol), s.ramp(both.outputs[0], 0.2, 1.4))
    col = darken(s, col, n, 0.4)
    return s.finish(col, both.outputs[0], 0.3, 0.02)


def shift_value(h, f):
    r, g, b = hexrgb(h)
    return "#%02x%02x%02x" % (int(min(1, r * f) * 255), int(min(1, g * f) * 255), int(min(1, b * f) * 255))


# ---------------------------------------------------------------- baking
def make_seamless(a):
    """Cross-fade the picture with a copy shifted by half its size so the edges meet."""
    h, w = a.shape[:2]
    x = np.linspace(0, 1, w)
    y = np.linspace(0, 1, h)
    wx = 1 - np.abs(2 * x - 1) ** 6
    wy = 1 - np.abs(2 * y - 1) ** 6
    m = (wy[:, None] * wx[None, :])[..., None]
    rolled = np.roll(np.roll(a, h // 2, axis=0), w // 2, axis=1)
    out = a * m + rolled * (1 - m)
    out[..., 3] = 1.0
    return out


def bake(mat, kind, size, path, seamless=True):  # seamless: cross-fade (only for noise-like surfaces)
    """Bake the material of a flat square to a PNG. kind: COLOR (diffuse colour) or NORMAL (tangent space)."""
    bpy.ops.mesh.primitive_plane_add(size=2)
    obj = bpy.context.active_object
    obj.data.materials.append(mat)
    img = bpy.data.images.new(os.path.basename(path), size, size, alpha=True)
    img.colorspace_settings.name = "sRGB" if kind == "COLOR" else "Non-Color"
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    mat.node_tree.nodes.active = tex
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    if kind == "COLOR":
        bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"}, margin=0)
    else:
        bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", margin=0)
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    a = px.reshape(size, size, 4)
    if seamless:
        a = make_seamless(a)
        if kind == "NORMAL":
            v = a[..., :3] * 2 - 1
            v /= np.maximum(np.linalg.norm(v, axis=2, keepdims=True), 1e-6)
            a[..., :3] = v * 0.5 + 0.5
    img.pixels.foreach_set(np.ascontiguousarray(a, dtype=np.float32).ravel())
    img.update()
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()
    mat.node_tree.nodes.remove(tex)
    bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.images.remove(img)


def build_textures(out, theme_ids, variant, size, log):
    files = {}
    for tid in theme_ids:
        th = THEMES[tid]
        files[tid] = {}
        for surf in ("floor", "wall"):
            kind, pal = th[surf]
            pal = [shift(c, variant) for c in pal]
            base = os.path.join(out, "textures", f"{tid}_{surf}_v{variant}")
            reset()
            mat = build_surface(kind, pal, seed_of(f"{tid}/{surf}/{variant}"), f"{tid}_{surf}")
            tile = kind in ("cobble", "flagstone", "moss")
            bake(mat, "COLOR", size, base + "_d.png", tile)
            bake(mat, "NORMAL", size, base + "_n.png", tile)
            files[tid][surf] = {"d": f"textures/{tid}_{surf}_v{variant}_d.png", "n": f"textures/{tid}_{surf}_v{variant}_n.png", "kind": kind}
            log(f"texture {tid} {surf} v{variant} ({kind})")
    return files


# ---------------------------------------------------------------- characters
class Builder:
    def __init__(self, s, b):
        self.s = s  # height factor
        self.b = b  # width factor
        self.parts = []

    def P(self, x, y, z):
        return (x * self.b * self.s, y * self.s, z * self.s)

    def add(self, kind, loc, dim, mat, bone, rot=(0, 0, 0), minor=0.03):
        loc = self.P(*loc)
        if kind == "cube":
            bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
        elif kind == "sphere":
            bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=10, radius=0.5, location=loc)
        elif kind == "cyl":
            bpy.ops.mesh.primitive_cylinder_add(vertices=14, radius=0.5, depth=1, location=loc)
        elif kind == "cone":
            bpy.ops.mesh.primitive_cone_add(vertices=14, radius1=0.5, radius2=0, depth=1, location=loc)
        else:
            bpy.ops.mesh.primitive_torus_add(major_radius=0.5, minor_radius=minor, major_segments=22, minor_segments=6, location=loc)
        o = bpy.context.active_object
        o.scale = (dim[0] * self.b * self.s, dim[1] * self.s, dim[2] * self.s)
        o.rotation_euler = rot
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        if kind in ("sphere", "cyl", "cone", "torus"):
            for p in o.data.polygons:
                p.use_smooth = True
        o.data.materials.append(mat)
        vg = o.vertex_groups.new(name=bone)
        vg.add(list(range(len(o.data.vertices))), 1.0, "REPLACE")
        self.parts.append(o)
        return o


def flat(name, hexcol, metallic=0.0, rough=0.7, emit=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = hexc(hexcol)
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = rough
    if emit:
        b.inputs["Emission Color"].default_value = hexc(emit)
        b.inputs["Emission Strength"].default_value = 2.0
    return m


def textured(name, path):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = bpy.data.images.load(path)
    nt.links.new(t.outputs["Color"], b.inputs["Base Color"])
    b.inputs["Roughness"].default_value = 0.85
    return m


def make_rig(s, b):
    arm = bpy.data.armatures.new("Rig")
    rig = bpy.data.objects.new("Rig", arm)
    bpy.context.scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")

    def bone(name, head, tail, parent=None):
        e = arm.edit_bones.new(name)
        e.head = (head[0] * b * s, head[1] * s, head[2] * s)
        e.tail = (tail[0] * b * s, tail[1] * s, tail[2] * s)
        if parent:
            e.parent = arm.edit_bones[parent]
        return e

    bone("Hips", (0, 0, 0.95), (0, 0, 1.05))
    bone("Spine", (0, 0, 1.05), (0, 0, 1.40), "Hips")
    bone("Head", (0, 0, 1.40), (0, 0, 1.72), "Spine")
    bone("ArmL", (0.24, 0, 1.38), (0.28, 0, 0.98), "Spine")
    bone("ArmR", (-0.24, 0, 1.38), (-0.28, 0, 0.98), "Spine")
    bone("LegL", (0.10, 0, 0.95), (0.10, 0, 0.05), "Hips")
    bone("LegR", (-0.10, 0, 0.95), (-0.10, 0, 0.05), "Hips")
    bpy.ops.object.mode_set(mode="OBJECT")
    return rig


def animate(rig):
    fps = bpy.context.scene.render.fps = 24
    pb = rig.pose.bones
    for p in pb:
        p.rotation_mode = "XYZ"
    rig.animation_data_create()

    def clear():
        for p in pb:
            p.rotation_euler = (0, 0, 0)

    def key(frame, vals):
        clear()
        for name, rot in vals.items():
            pb[name].rotation_euler = rot
        for name in vals:
            pb[name].keyframe_insert("rotation_euler", frame=frame)

    def push(name):
        act = rig.animation_data.action
        act.name = name
        tr = rig.animation_data.nla_tracks.new()
        tr.name = name
        tr.strips.new(name, int(act.frame_range[0]), act)
        rig.animation_data.action = None

    # idle: slow breathing, a little sway
    for f, k in ((1, 0.0), (13, 1.0), (25, 0.0), (37, -0.6), (49, 0.0)):
        key(f, {"Spine": (0.025 * k, 0, 0.02 * k), "Head": (0.02 * k, 0.05 * k, 0), "ArmL": (0, 0, 0.04 * k), "ArmR": (0, 0, -0.04 * k)})
    push("Idle")
    # walk: legs and arms swing against each other
    for f, k in ((1, 1.0), (7, 0.0), (13, -1.0), (19, 0.0), (25, 1.0)):
        key(f, {"LegL": (0.55 * k, 0, 0), "LegR": (-0.55 * k, 0, 0), "ArmL": (-0.4 * k, 0, 0.03), "ArmR": (0.4 * k, 0, -0.03), "Spine": (0, 0.04 * k, 0), "Hips": (0, -0.04 * k, 0)})
    push("Walk")
    clear()
    return fps


def build_character(c, theme, out, size, log):
    cid = c["id"]
    rnd = random.Random(seed_of(cid))
    cls = c.get("class", "fighter")
    st = CLASS_STYLE.get(cls, CLASS_STYLE["fighter"])
    scores = c.get("scores", [10] * 6)
    height = round(1.62 + rnd.random() * 0.25, 2)
    bulk = max(0.85, min(1.22, 0.9 + (scores[0] + scores[2] - 20) / 40))
    s = height / 1.75
    skin, hair = rnd.choice(SKIN), rnd.choice(HAIR)
    cloth = shift(st["cloth"], 1 + rnd.randint(0, 3))
    scifi = theme["scifi"]
    sc = reset()
    tex_path = os.path.join(out, "textures", f"char_{cid}.png")
    cloth_mat = weave(cloth, seed_of(cid), "cloth")
    bake(cloth_mat, "COLOR", size, tex_path, seamless=False)
    reset()  # fresh scene; keep the baked PNG
    cloth_m = textured("cloth", tex_path)
    skin_m, hair_m = flat("skin", skin, 0, 0.6), flat("hair", hair, 0, 0.8)
    trim_m = flat("trim", st["trim"], 0.6, 0.4)
    metal_m = flat("metal", "#aab0ba", 0.9, 0.3)
    accent_m = flat("accent", theme["accent"], 0, 0.5, emit=theme["accent"] if scifi else None)
    dark_m = flat("dark", "#15151a", 0, 0.6)
    B = Builder(s, bulk)
    A = B.add
    # body
    A("cube", (0, 0, 1.22), (0.40, 0.22, 0.34), cloth_m, "Spine")
    A("cube", (0, 0, 0.98), (0.36, 0.20, 0.16), trim_m, "Hips")
    A("sphere", (0, 0, 1.58), (0.21, 0.23, 0.25), skin_m, "Head")
    A("sphere", (0.045, -0.095, 1.60), (0.035, 0.03, 0.035), dark_m, "Head")
    A("sphere", (-0.045, -0.095, 1.60), (0.035, 0.03, 0.035), dark_m, "Head")
    A("cyl", (0, 0, 1.43), (0.10, 0.10, 0.08), skin_m, "Head")
    for sx, arm in ((1, "ArmL"), (-1, "ArmR")):
        A("cyl", (0.26 * sx, 0, 1.22), (0.09, 0.09, 0.34), cloth_m, arm)
        A("sphere", (0.28 * sx, 0, 0.98), (0.09, 0.09, 0.09), skin_m, arm)
        A("sphere", (0.25 * sx, 0, 1.38), (0.12, 0.12, 0.12), trim_m, arm)
    for sx, leg in ((1, "LegL"), (-1, "LegR")):
        A("cyl", (0.10 * sx, 0, 0.50), (0.14, 0.14, 0.90), cloth_m, leg)
        A("cube", (0.10 * sx, -0.04, 0.06), (0.14, 0.26, 0.10), dark_m, leg)
    # head gear and hair
    if scifi:
        A("sphere", (0, 0, 1.60), (0.25, 0.26, 0.27), metal_m, "Head")
        A("cube", (0, -0.12, 1.60), (0.20, 0.06, 0.08), accent_m, "Head")
    elif cls == "wizard":
        A("cyl", (0, 0, 1.70), (0.46, 0.46, 0.02), cloth_m, "Head")
        A("cone", (0, 0, 1.92), (0.30, 0.30, 0.44), cloth_m, "Head")
    elif cls in ("rogue", "ranger"):
        A("sphere", (0, 0.02, 1.61), (0.25, 0.27, 0.27), cloth_m, "Head")
    elif cls == "barbarian":
        A("sphere", (0, 0.03, 1.66), (0.23, 0.22, 0.14), hair_m, "Head")
        A("torus", (0, 0, 1.44), (0.42, 0.42, 0.42), hair_m, "Spine", minor=0.07)
    elif cls == "fighter":
        A("sphere", (0, 0, 1.63), (0.24, 0.25, 0.20), metal_m, "Head")
    else:
        A("sphere", (0, 0.03, 1.64), (0.22, 0.22, 0.17), hair_m, "Head")
        A("torus", (0, 0, 1.68), (0.26, 0.26, 0.26), trim_m, "Head", minor=0.025)
    # what they carry (right hand is ArmR, at x = -0.28)
    hand = (-0.28, -0.06, 0.98)
    prop = st["prop"]
    if scifi:
        A("cube", (hand[0], hand[1] - 0.10, hand[2] + 0.03), (0.06, 0.34, 0.10), metal_m, "ArmR")
        A("cube", (hand[0], hand[1] - 0.28, hand[2] + 0.04), (0.04, 0.05, 0.04), accent_m, "ArmR")
    elif prop == "sword":
        A("cube", (hand[0], hand[1], hand[2] + 0.42), (0.05, 0.015, 0.8), metal_m, "ArmR")
        A("cube", (hand[0], hand[1], hand[2] + 0.02), (0.22, 0.04, 0.04), trim_m, "ArmR")
        A("cyl", (0.34, -0.06, 1.10), (0.42, 0.42, 0.04), trim_m, "ArmL", rot=(math.pi / 2, 0, 0))
    elif prop == "staff":
        A("cyl", (hand[0], hand[1], 1.05), (0.045, 0.045, 1.9), dark_m, "ArmR")
        A("sphere", (hand[0], hand[1], 2.02), (0.13, 0.13, 0.13), accent_m, "ArmR")
    elif prop == "mace":
        A("cyl", (hand[0], hand[1], hand[2] + 0.18), (0.05, 0.05, 0.5), dark_m, "ArmR")
        A("sphere", (hand[0], hand[1], hand[2] + 0.46), (0.16, 0.16, 0.16), metal_m, "ArmR")
        A("cube", (0, -0.12, 1.28), (0.10, 0.02, 0.14), trim_m, "Spine")
    elif prop == "dagger":
        A("cube", (hand[0], hand[1], hand[2] + 0.18), (0.035, 0.012, 0.34), metal_m, "ArmR")
        A("cube", (0.28, -0.06, 1.00), (0.035, 0.012, 0.30), metal_m, "ArmL")
    elif prop == "bow":
        A("torus", (hand[0] - 0.02, hand[1], hand[2] + 0.30), (0.9, 0.9, 0.9), trim_m, "ArmR", rot=(0, math.pi / 2, 0), minor=0.015)
        A("cyl", (0.08, 0.14, 1.28), (0.09, 0.09, 0.5), trim_m, "Spine", rot=(0.2, 0, 0))
    else:  # axe
        A("cyl", (hand[0], hand[1], hand[2] + 0.22), (0.05, 0.05, 0.9), dark_m, "ArmR")
        A("cube", (hand[0] - 0.12, hand[1], hand[2] + 0.62), (0.22, 0.03, 0.26), metal_m, "ArmR")
    # one mesh, skinned to the rig
    bpy.ops.object.select_all(action="DESELECT")
    for o in B.parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = B.parts[0]
    bpy.ops.object.join()
    body = bpy.context.active_object
    body.name = cid
    rig = make_rig(s, bulk)
    mod = body.modifiers.new("Armature", "ARMATURE")
    mod.object = rig
    body.parent = rig
    animate(rig)
    rig.name = cid + "_rig"
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    body.select_set(True)
    bpy.context.view_layer.objects.active = rig
    path = os.path.join(out, "models", f"{cid}.glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_animations=True, export_apply=False, export_yup=True)
    log(f"model {cid} ({cls}, {height} m)")
    return {"model": f"models/{cid}.glb", "cloth": f"textures/char_{cid}.png", "height": height}


# ---------------------------------------------------------------- driver
def load_cast(data):
    cast = []
    for f, mod in [(os.path.join(data, "characters.json"), None)] + [(os.path.join(data, "modules", d, "characters.json"), d) for d in sorted(os.listdir(os.path.join(data, "modules"))) if os.path.isdir(os.path.join(data, "modules", d))]:
        if os.path.exists(f):
            for c in json.load(open(f, encoding="utf-8")):
                c = dict(c)
                c["module"] = mod
                cast.append(c)
    return cast


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default="sim/data")
    ap.add_argument("--out", default="sim/web/assets")
    ap.add_argument("--size", type=int, default=256, help="surface texture size")
    ap.add_argument("--char-size", type=int, default=128, help="cloth texture size")
    ap.add_argument("--variant", type=int, default=0, help="texture variant to build (default 1, or the next one with --new-textures)")
    ap.add_argument("--new-textures", action="store_true", help="add one more variant of every theme")
    ap.add_argument("--rebuild-characters", action="store_true", help="rewrite models that already exist")
    ap.add_argument("--only", default="", help="textures or characters")
    ap.add_argument("--themes", default="", help="comma separated theme ids")
    a = ap.parse_args(argv)
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    os.makedirs(os.path.join(out, "models"), exist_ok=True)
    mpath = os.path.join(out, "manifest.json")
    man = json.load(open(mpath, encoding="utf-8")) if os.path.exists(mpath) else {}
    man.setdefault("themes", {})
    man.setdefault("characters", [])
    log = lambda m: print("gen_assets:", m, flush=True)

    modules = {}
    for fn in os.listdir(os.path.join(a.data, "modules")):
        if fn.endswith(".json"):
            try:
                modules[fn[:-5]] = json.load(open(os.path.join(a.data, "modules", fn), encoding="utf-8")).get("name", fn[:-5])
            except Exception:
                pass
    for mid in modules:
        THEMES.setdefault(mid, dict(THEMES["classic"], label=modules[mid]))
    ids = [t for t in (a.themes.split(",") if a.themes else THEMES) if t in THEMES]

    if a.only != "characters":
        for tid in ids:
            have = [int(v) for v in man["themes"].get(tid, {}).get("variants", {})]
            v = a.variant or ((max(have) + 1) if (a.new_textures and have) else 1)
            if v in have and not a.variant:
                continue
            files = build_textures(out, [tid], v, a.size, log)[tid]
            t = man["themes"].setdefault(tid, {"label": THEMES[tid]["label"], "accent": THEMES[tid]["accent"], "variants": {}})
            t["label"], t["accent"] = THEMES[tid]["label"], THEMES[tid]["accent"]
            t["variants"][str(v)] = files

    if a.only != "textures":
        done = {c["id"]: c for c in man["characters"]}
        for c in load_cast(a.data):
            mid = c["module"] or "classic"
            path = os.path.join(out, "models", c["id"] + ".glb")
            if os.path.exists(path) and not a.rebuild_characters and c["id"] in done:
                continue
            files = build_character(c, THEMES.get(mid, THEMES["classic"]), out, a.char_size, log)
            done[c["id"]] = dict(id=c["id"], name=c["name"], cls=c.get("class", ""), module=mid, tags=c.get("tags", []), **files)
        man["characters"] = list(done.values())
    man["version"] = 1
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
    log(f"wrote {mpath}")


if __name__ == "__main__":
    argv = sys.argv
    main(argv[argv.index("--") + 1:] if "--" in argv else argv[1:])
