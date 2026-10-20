### Fixed

- `orlyc`: emit `Rt::Match` instead of `Rt::EqEq` in generated variant `EqEq` methods for arms whose payload is or contains an optional (`TOpt`), fixing a C++ compilation failure (`cannot convert 'Orly::Rt::TOpt<bool>' to 'bool' in return`) when comparing variants with optional-payload arms (#972).
