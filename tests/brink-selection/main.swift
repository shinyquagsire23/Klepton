import simd
func require(_ condition: Bool, _ message: String) { precondition(condition,message);print("PASS \(message)") }
var tap=KLBrinkSelectionGesture()
require(tap.update(translation:.zero,ended:false,cancelled:false)==nil,"pinch start does not click")
if case .tap?=tap.update(translation:SIMD2(0.005,0.003),ended:true,cancelled:false) {} else { fatalError("tap missing") }
print("PASS release clicks once with small hand jitter")
var drag=KLBrinkSelectionGesture()
if case .swipe?=drag.update(translation:SIMD2(-0.05,0),ended:false,cancelled:false) {} else { fatalError("swipe missing") }
require(drag.update(translation:SIMD2(-0.05,0),ended:true,cancelled:false)==nil,"a swipe never becomes a release tap")
var cancel=KLBrinkSelectionGesture()
require(cancel.update(translation:SIMD2(0.1,0),ended:true,cancelled:true)==nil,"cancelled pinch does not activate anything")
var invalid=KLBrinkSelectionGesture()
require(invalid.update(translation:SIMD2(.nan,0),ended:true,cancelled:false)==nil,"invalid hand data does not click")
