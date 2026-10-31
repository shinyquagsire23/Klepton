import simd

for hand in 0...1 {
 var p = [Float](repeating: 0, count: 208)
 let mirror: Float = hand == 0 ? 1 : -1
 for j in 0..<26 { p[j*8+6]=1; p[j*8+7]=0.009 }
 func put(_ j:Int,_ x:Float,_ z:Float) { p[j*8]=x*mirror; p[j*8+2]=z }
 put(1,0,0)
 for start in [2,6,11,16,21] {
  let x:Float = start == 2 ? 0.065 : start == 6 ? 0.03 : start == 11 ? 0 : start == 16 ? -0.025 : -0.05
  let count = start == 2 ? 4 : 5
  for i in 0..<count { put(start+i,x,-0.02-Float(i)*0.025) }
 }
 var flags=[UInt32](repeating:3,count:26), axes=[Int](repeating:-1,count:26)
 let original=p
 precondition(klConvertOpenXRHand(&p,flags:&flags,hand:hand,axes:&axes))
 precondition(abs(p[2] - (original[11*8+2]+original[12*8+2])*0.5) < 1e-6)
 for j in 0..<26 {
  let k=j*8
  let q=simd_quatf(ix:p[k+3],iy:p[k+4],iz:p[k+5],r:p[k+6])
  precondition(abs(simd_length(q.vector)-1)<1e-5)
  precondition(simd_dot(q.act(SIMD3<Float>(0,1,0)),SIMD3<Float>(0,1,0)) > 0.99)
  precondition(simd_dot(q.act(SIMD3<Float>(0,0,1)),SIMD3<Float>(0,0,1)) > 0.99)
  precondition(flags[j] == 3)
 }
 // Rigidly rotate/translate the whole tracked hand: result must transform identically.
 var moved=original
 let turn=simd_quatf(angle:1.1,axis:simd_normalize(SIMD3<Float>(1,2,3)))
 for j in 0..<26 {
  let k=j*8
  let v=turn.act(SIMD3<Float>(moved[k],moved[k+1],moved[k+2]))+SIMD3<Float>(0.3,0.8,-0.7)
  moved[k]=v.x; moved[k+1]=v.y; moved[k+2]=v.z
  moved[k+3]=turn.imag.x; moved[k+4]=turn.imag.y; moved[k+5]=turn.imag.z; moved[k+6]=turn.real
 }
 precondition(klConvertOpenXRHand(&moved,flags:&flags,hand:hand,axes:&axes))
 for j in 0..<26 {
  let k=j*8
  let q=simd_quatf(ix:moved[k+3],iy:moved[k+4],iz:moved[k+5],r:moved[k+6])
  precondition(simd_length(q.act(SIMD3<Float>(0,1,0))-turn.act(SIMD3<Float>(0,1,0))) < 1e-5)
 }
 var curled=original
 let curl=simd_quatf(angle: -.pi/2, axis: SIMD3<Float>(1,0,0))
 let pivot=SIMD3<Float>(original[56],original[57],original[58])
 for j in 7...10 {
  let k=j*8
  let v=pivot+curl.act(SIMD3<Float>(original[k],original[k+1],original[k+2])-pivot)
  curled[k]=v.x; curled[k+1]=v.y; curled[k+2]=v.z
  curled[k+3]=curl.imag.x; curled[k+4]=curl.imag.y; curled[k+5]=curl.imag.z; curled[k+6]=curl.real
 }
 precondition(klConvertOpenXRHand(&curled,flags:&flags,hand:hand,axes:&axes))
 for j in 7...10 {
  let k=j*8, q=simd_quatf(ix:curled[k+3],iy:curled[k+4],iz:curled[k+5],r:curled[k+6])
  precondition(simd_length(q.act(SIMD3<Float>(0,1,0))-curl.act(SIMD3<Float>(0,1,0))) < 1e-5)
 }
 print("PASS curled index: retained dorsal axis and fingertip orientation")
 moved[8] = .nan
 precondition(!klConvertOpenXRHand(&moved,flags:&flags,hand:hand,axes:&axes))
 print("PASS hand \(hand): axes, palm center, inferred validity, rigid transforms, invalid input")
}
