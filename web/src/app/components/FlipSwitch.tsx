import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: React.ReactNode;
  checked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
  checkedChildren?: React.ReactNode;
  unCheckedChildren?: React.ReactNode;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({
  label,
  checked,
  disabled,
  onChange,
  checkedChildren,
  unCheckedChildren
}) => {
  const switchRef = React.useRef<HTMLButtonElement>(null);

  const handleToggle = (e: React.MouseEvent) => {
    // Delegate to Ant Design Switch's native click handler via ref.
    // This avoids manually calling onChange(!checked), which bypasses
    // Ant Design's state management and can cause double-call issues
    // when the browser also forwards the label click to the associated element.
    e.preventDefault();
    if (!disabled) {
      switchRef.current?.click();
    }
  };

  return (
    <label
      onClick={handleToggle}
      style={{
        cursor: disabled ? 'not-allowed' : 'pointer',
        display: 'inline-flex',
        alignItems: 'center',
        gap: 8,
      }}
    >
      {label && <Text disabled={disabled}>{label}</Text>}
      <Switch
        ref={switchRef}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        checkedChildren={checkedChildren}
        unCheckedChildren={unCheckedChildren}
        aria-label={typeof label === 'string' ? label : undefined}
      />
    </label>
  );
};

export default FlipSwitch;
