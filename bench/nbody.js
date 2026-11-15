const PI = Math.PI, SOLAR = 4 * PI * PI, DPY = 365.24
const bs = [
  [0,0,0,0,0,0,SOLAR],
  [4.84143144246472090e+00,-1.16032004402742839e+00,-1.03622044471123109e-01,1.66007664274403694e-03*DPY,7.69901118419740425e-03*DPY,-6.90460016972063023e-05*DPY,9.54791938424326609e-04*SOLAR],
  [8.34336671824457987e+00,4.12479856412430479e+00,-4.03523417114321381e-01,-2.76742510726862411e-03*DPY,4.99852801234917238e-03*DPY,2.30417297573763929e-05*DPY,2.85885980666130812e-04*SOLAR],
  [1.28943695621391310e+01,-1.51111514016986312e+01,-2.23307578892655734e-01,2.96460137564761618e-03*DPY,2.37847173959480950e-03*DPY,-2.96589568540237556e-05*DPY,4.36624404335156298e-05*SOLAR],
  [1.53796971148509165e+01,-2.59193146099879641e+01,1.79258772950371181e-01,2.68067772490389322e-03*DPY,1.62824170038242295e-03*DPY,-9.51592254519715870e-05*DPY,5.15138902046611451e-05*SOLAR],
].map(([x,y,z,vx,vy,vz,m]) => ({x,y,z,vx,vy,vz,m}))
function advance(dt) {
  for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) {
    const a = bs[i], b = bs[j], dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z
    const d2 = dx*dx+dy*dy+dz*dz, mag = dt / (d2 * Math.sqrt(d2)), mj = b.m*mag, mi = a.m*mag
    a.vx -= dx*mj; a.vy -= dy*mj; a.vz -= dz*mj; b.vx += dx*mi; b.vy += dy*mi; b.vz += dz*mi
  }
  for (const b of bs) { b.x += dt*b.vx; b.y += dt*b.vy; b.z += dt*b.vz }
}
function energy() {
  let e = 0
  for (let i = 0; i < bs.length; i++) { const b = bs[i]
    e += 0.5*b.m*(b.vx*b.vx+b.vy*b.vy+b.vz*b.vz)
    for (let j = i+1; j < bs.length; j++) { const c = bs[j], dx=b.x-c.x, dy=b.y-c.y, dz=b.z-c.z; e -= b.m*c.m/Math.sqrt(dx*dx+dy*dy+dz*dz) } }
  return e
}
let px=0,py=0,pz=0; for (const b of bs) { px+=b.vx*b.m; py+=b.vy*b.m; pz+=b.vz*b.m }
bs[0].vx=-px/SOLAR; bs[0].vy=-py/SOLAR; bs[0].vz=-pz/SOLAR
console.log(energy()); for (let i = 0; i < 20_000_000; i++) advance(0.01); console.log(energy())
