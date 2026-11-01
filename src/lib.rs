//! Standalone Poisson healing core adapted from PhotoCraft 0.18.0's
//! crates/algo/src/poisson.rs (MIT OR Apache-2.0).
//! Input: interleaved RGBA8 source/destination, byte mask (nonzero = heal).
//! Source and destination are equal-sized rectangular buffers.
use std::slice;

const TOL: f32 = 2.0e-5;
const FINE_ITERS: usize = 30;
const FINE_OMEGA: f32 = 1.7;

fn sor(w: usize, h: usize, unknown: &[bool], v: &mut [f32], max_iter: usize, omega: f32) {
    let cells: Vec<(usize, u8)> = (0..w*h).filter(|&i| unknown[i]).map(|i| {
        let (x,y)=(i%w,i/w);
        (i, u8::from(x>0) | (u8::from(x+1<w)<<1) |
            (u8::from(y>0)<<2) | (u8::from(y+1<h)<<3))
    }).collect();
    for _ in 0..max_iter {
        let mut max_d=0.0f32;
        for &(i,m) in &cells {
            let (mut s,mut n)=(0.0f32,0.0f32);
            if m&1!=0 {s+=v[i-1]; n+=1.0;}
            if m&2!=0 {s+=v[i+1]; n+=1.0;}
            if m&4!=0 {s+=v[i-w]; n+=1.0;}
            if m&8!=0 {s+=v[i+w]; n+=1.0;}
            if n==0.0 {continue;}
            let d=omega*(s/n-v[i]); v[i]+=d; max_d=max_d.max(d.abs());
        }
        if max_d<TOL {break;}
    }
}
fn solve_rec(w:usize,h:usize,u:&[bool],v:&mut[f32],depth:usize) {
    let n=u.iter().filter(|&&b|b).count();
    if n==0 {return;}
    let known:Vec<f32>=v.iter().zip(u).filter(|(_,b)|!**b).map(|(x,_)|*x).collect();
    if known.is_empty(){return;}
    if w>=8 && h>=8 && n>64 && depth<16 {
        let (cw,ch)=((w+1)/2,(h+1)/2);
        let mut cu=vec![true;cw*ch]; let mut cv=vec![0.0;cw*ch];
        for cy in 0..ch { for cx in 0..cw {
            let (mut s,mut count)=(0.0f32,0u32);
            for (dx,dy) in [(0,0),(1,0),(0,1),(1,1)] {
                let (x,y)=(cx*2+dx,cy*2+dy);
                if x<w && y<h && !u[y*w+x] {s+=v[y*w+x];count+=1;}
            }
            if count>0 {cu[cy*cw+cx]=false;cv[cy*cw+cx]=s/count as f32;}
        }}
        solve_rec(cw,ch,&cu,&mut cv,depth+1);
        for y in 0..h {
            let fy=((y as f32+0.5)/2.0-0.5).clamp(0.0,(ch-1) as f32);
            let y0=fy.floor() as usize; let y1=(y0+1).min(ch-1); let ty=fy-y0 as f32;
            for x in 0..w { if !u[y*w+x] {continue;}
                let fx=((x as f32+0.5)/2.0-0.5).clamp(0.0,(cw-1) as f32);
                let x0=fx.floor() as usize; let x1=(x0+1).min(cw-1); let tx=fx-x0 as f32;
                let a=cv[y0*cw+x0]+(cv[y0*cw+x1]-cv[y0*cw+x0])*tx;
                let b=cv[y1*cw+x0]+(cv[y1*cw+x1]-cv[y1*cw+x0])*tx;
                v[y*w+x]=a+(b-a)*ty;
            }
        }
        sor(w,h,u,v,FINE_ITERS,FINE_OMEGA);
    } else {
        let mean=known.iter().sum::<f32>()/known.len() as f32;
        for (x,&unknown) in v.iter_mut().zip(u) {if unknown {*x=mean;}}
        sor(w,h,u,v,2000,1.85);
    }
}
fn solve(w:usize,h:usize,u:&[bool],v:&mut[f32]) { solve_rec(w,h,u,v,0); }

/// Poisson seamless-clone RGBA8 image buffers.
/// Returns 0 on success, -1 on invalid arguments, -2 on panic.
/// `src`/`dst` are width*height*4 bytes; `mask` is width*height bytes.
/// Pixels outside mask are copied unchanged from dst. Keep a 1px unmasked border.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fdh_heal_rgba8(
    width:u32, height:u32, src:*const u8, dst:*const u8,
    mask:*const u8, output:*mut u8
)->i32 {
    std::panic::catch_unwind(|| {
        let (w,h)=(width as usize,height as usize);
        if w==0 || h==0 || w.checked_mul(h).is_none() ||
            w*h>100_000_000 || src.is_null() || dst.is_null() ||
            mask.is_null() || output.is_null() {return -1;}
        let n=w*h;
        let s=slice::from_raw_parts(src,n*4);
        let d=slice::from_raw_parts(dst,n*4);
        let m=slice::from_raw_parts(mask,n);
        let unknown:Vec<bool>=m.iter().map(|&x|x!=0).collect();
        let mut out=d.to_vec();
        for c in 0..4 {
            let mut v:Vec<f32>=(0..n).map(|i| if unknown[i] {0.0} else {(d[i*4+c] as f32)/255.0-(s[i*4+c] as f32)/255.0}).collect();
            solve(w,h,&unknown,&mut v);
            for i in 0..n { if unknown[i] {
                let healed=(s[i*4+c] as f32)/255.0+v[i];
                out[i*4+c]=(healed.clamp(0.0,1.0)*255.0+0.5) as u8;
            }}
        }
        std::ptr::copy_nonoverlapping(out.as_ptr(),output,n*4);
        0
    }).unwrap_or(-2)
}

/// PhotoCraft Remove Tool core: automatically complete a painted-over region from its surroundings.
/// Input/output are RGBA8; mask is one byte per pixel. The algorithm closes enclosed loops,
/// grows the hole by two pixels, then runs PhotoCraft's patch-based non-local completion.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fdh_remove_rgba8(
    width: u32,
    height: u32,
    image: *const u8,
    mask: *const u8,
    output: *mut u8,
) -> i32 {
    std::panic::catch_unwind(|| {
        let (w, h) = (width as usize, height as usize);
        let Some(n) = w.checked_mul(h) else { return -1; };
        if w < 7 || h < 7 || n == 0 || n > 16_777_216 ||
            image.is_null() || mask.is_null() || output.is_null() {
            return -1;
        }
        let Some(bytes) = n.checked_mul(4) else { return -1; };
        // SAFETY: pointers and lengths are validated above; caller guarantees buffers are valid.
        let src = unsafe { slice::from_raw_parts(image, bytes) };
        let mask_bytes = unsafe { slice::from_raw_parts(mask, n) };
        let dst = unsafe { slice::from_raw_parts_mut(output, bytes) };

        let mut hole: Vec<bool> = mask_bytes.iter().map(|&v| v != 0).collect();
        if !hole.iter().any(|&v| v) { dst.copy_from_slice(src); return 0; }
        hole = photocraft_algo::remove::close_loops(w, h, &hole);
        hole = photocraft_algo::remove::dilate(w, h, &hole, 2);

        let floats: Vec<f32> = src.iter().map(|&v| f32::from(v) / 255.0).collect();
        match photocraft_algo::remove::remove_with(
            w, h, 4, &floats, &hole, 0x0052_e30e, &photocraft_raster::Interrupt::NONE,
        ) {
            Ok(result) => {
                for (d, s) in dst.iter_mut().zip(result.iter()) {
                    *d = (s.clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
                }
                0
            }
            Err(_) => -3,
        }
    }).unwrap_or(-2)
}
