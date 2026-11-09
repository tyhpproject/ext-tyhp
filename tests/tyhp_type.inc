<?php

namespace Tyhp;

function show(string $label, mixed $value): void
{
    echo $label, '=';
    if ($value === null) {
        echo "null\n";
        return;
    }
    if ($value === true) {
        echo "true\n";
        return;
    }
    if ($value === false) {
        echo "false\n";
        return;
    }
    echo "other\n";
}

/**
 * Stand-in for the runtime Type. Fields match the real class so the extension
 * can read them by name. Public visibility is enough; the walker also reads
 * private properties of the same names.
 */
class Type
{
    public function __construct(
        public string $kind,
        public ?string $name = null,
        public array $typeArgs = [],
        public bool $isNullable = false,
        public array $structFields = [],
        public array $structRequiredKeys = [],
        public ?self $forward = null,
        public ?self $lazyInner = null,
        public bool $resolving = false,
        public string $lazyOp = '',
        public ?string $lazyName = null,
        public bool $placeholder = false,
    ) {}

    public static function scalar(string $name, bool $isNullable = false): self
    {
        if ($name === 'null') {
            $isNullable = true;
        }

        return new self('scalar', $name, isNullable: $isNullable);
    }

    /** One interned int, so BuiltIn::toType() and Type::int() are the same object. */
    public static function int(): self
    {
        return self::$intSingleton ??= self::scalar('int');
    }

    /**
     * Interned the way Type::fromClassName does: the key is class:$className
     * and the same name returns the same instance.
     */
    public static function fromClassName(string $className): self
    {
        return self::$classSingletons['class:' . $className] ??= new self('class', $className);
    }

    private static ?self $intSingleton = null;

    /** @var array<string, self> */
    private static array $classSingletons = [];
}

/**
 * Stand-in for the runtime BuiltIn enum. toType() returns the Type::int()
 * singleton so the bind cache can share that slot.
 */
enum BuiltIn
{
    case int;

    public function toType(): Type
    {
        return Type::int();
    }
}

/* The real NamedType extends Type: kind 'named', name is the parameter name,
 * and it is nullable when the underlying type is. */
final class NamedType extends Type
{
    public function __construct(
        public string $parameterName,
        public Type $underlyingType,
    ) {
        parent::__construct('named', $parameterName, [], $underlyingType->isNullable);
    }
}
